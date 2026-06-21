#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/syscall.h>

#include <glib.h>
#include <glib-unix.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <poll.h>
#include <time.h>

#include <wayland-client.h>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#ifndef ARRAY_LEN
#define ARRAY_LEN(x) (sizeof(x)/sizeof((x)[0]))
#endif

// ---------------- Logging ----------------
static void log_err(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

// ---------------- Config ----------------
static char *path_join(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  char *p = calloc(1, la + lb + 2);
  if (!p) return NULL;
  memcpy(p, a, la);
  if (la && a[la-1] != '/') p[la++] = '/';
  memcpy(p+la, b, lb);
  return p;
}

static char *expand_tilde(const char *in) {
  if (!in) return NULL;
  if (in[0] != '~') return strdup(in);
  const char *home = getenv("HOME");
  if (!home) home = "";
  if (in[1] == '\0') return strdup(home);
  if (in[1] == '/') {
    char *p = path_join(home, in+2);
    return p;
  }
  // ~user not supported
  return strdup(in);
}

static char *canonicalize_path(const char *in) {
  char *tmp = expand_tilde(in);
  if (!tmp) return NULL;
  char resolved[PATH_MAX];
  if (realpath(tmp, resolved)) {
    free(tmp);
    return strdup(resolved);
  }
  // If realpath fails (file missing), just return expanded input.
  return tmp;
}

static char *config_path(void) {
  const char *home = getenv("HOME");
  if (!home) home = "";
  return path_join(home, ".config/maxpaper/config");
}

static bool ensure_config_dir(void) {
  const char *home = getenv("HOME");
  if (!home) return false;
  char *dir = path_join(home, ".config/maxpaper");
  if (!dir) return false;
  int rc = mkdir(dir, 0755);
  if (rc != 0 && errno != EEXIST) {
    log_err("[maxpaper] mkdir %s failed: %s", dir, strerror(errno));
    free(dir);
    return false;
  }
  free(dir);
  return true;
}

static bool read_kv_file(const char *path, char **out_video, char **out_color) {
  FILE *f = fopen(path, "r");
  if (!f) return false;
  char line[4096];
  while (fgets(line, sizeof(line), f)) {
    // strip newline
    size_t n = strlen(line);
    while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
    if (n == 0 || line[0] == '#') continue;
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    const char *k = line;
    const char *v = eq + 1;
    if (strcmp(k, "VIDEO") == 0) {
      free(*out_video);
      *out_video = strdup(v);
    } else if (strcmp(k, "COLOR") == 0) {
      free(*out_color);
      *out_color = strdup(v);
    }
  }
  fclose(f);
  return true;
}

static bool write_kv_file(const char *path, const char *video, const char *color) {
  FILE *f = fopen(path, "w");
  if (!f) {
    log_err("[maxpaper] Cannot write config: %s", path);
    return false;
  }
  fprintf(f, "# MaxPaper config\n");
  fprintf(f, "VIDEO=%s\n", video ? video : "");
  fprintf(f, "COLOR=%s\n", color ? color : "#000000");
  fclose(f);
  return true;
}

static uint32_t parse_color_rgb(const char *s) {
  if (!s) return 0x000000;
  if (s[0] == '#') s++;
  if (strlen(s) != 6) return 0x000000;
  char buf[3] = {0,0,0};
  buf[0]=s[0]; buf[1]=s[1];
  int r = (int)strtol(buf, NULL, 16);
  buf[0]=s[2]; buf[1]=s[3];
  int g = (int)strtol(buf, NULL, 16);
  buf[0]=s[4]; buf[1]=s[5];
  int b = (int)strtol(buf, NULL, 16);
  return ((uint32_t)r<<16) | ((uint32_t)g<<8) | (uint32_t)b;
}

// ---------------- Wayland shm helpers ----------------
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef SYS_memfd_create
#if defined(__x86_64__)
#define SYS_memfd_create 319
#elif defined(__aarch64__)
#define SYS_memfd_create 279
#endif
#endif
static int memfd_create_compat(const char *name, unsigned int flags) {
#ifdef SYS_memfd_create
  return (int)syscall(SYS_memfd_create, name, flags);
#else
  (void)name; (void)flags;
  errno = ENOSYS;
  return -1;
#endif
}

static int create_shm_file(size_t size) {
  int fd = memfd_create_compat("maxpaper-shm", MFD_CLOEXEC);
  if (fd >= 0) {
    if (ftruncate(fd, (off_t)size) != 0) { close(fd); return -1; }
    return fd;
  }

  const char *runtime = getenv("XDG_RUNTIME_DIR");
  if (!runtime) runtime = "/tmp";
  char tmpl[PATH_MAX];
  snprintf(tmpl, sizeof(tmpl), "%s/maxpaper-shm-XXXXXX", runtime);
  fd = mkstemp(tmpl);
  if (fd < 0) return -1;
  unlink(tmpl);
  if (ftruncate(fd, (off_t)size) != 0) { close(fd); return -1; }
  return fd;
}

// ---------------- State ----------------
struct mp_state {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct zwlr_layer_shell_v1 *layer_shell;

  struct wl_output *output;
  int out_w, out_h;
  int out_scale;
  bool have_mode;
  bool have_scale;

  struct wl_surface *surface;
  struct zwlr_layer_surface_v1 *layer_surface;
  bool configured;

  // shm buffers
  int buf_w, buf_h;
  int stride;
  size_t shm_size;
  int shm_fd;
  void *shm_data;
  struct wl_shm_pool *pool;
  struct wl_buffer *buffers[2];
  bool buf_busy[2];
  int cur_buf;

  // rendering / config
  uint32_t bg_rgb; // 0xRRGGBB
  int target_fps;  // display-side frame cap (independent of MAXPAPER_OPT_FPS at encode time)

  // gstreamer
  GstElement *playbin;
  GstElement *appsink;
  GstElement *vfilter_bin;
  GMainLoop *loop;
  bool video_failed;

  char *video_path;
};

static void wl_buffer_release(void *data, struct wl_buffer *buf) {
  struct mp_state *st = (struct mp_state*)data;
  for (int i=0;i<2;i++) {
    if (st->buffers[i] == buf) st->buf_busy[i] = false;
  }
}
static const struct wl_buffer_listener wl_buffer_listener = {
  .release = wl_buffer_release
};

// wl_output listener (include name/description to avoid crash on newer versions)
static void output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
                            int32_t phys_w, int32_t phys_h, int32_t subpixel,
                            const char *make, const char *model, int32_t transform) {
  (void)data;(void)o;(void)x;(void)y;(void)phys_w;(void)phys_h;(void)subpixel;(void)make;(void)model;(void)transform;
}
static void output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
  (void)o;(void)refresh;
  struct mp_state *st = (struct mp_state*)data;
  if (flags & WL_OUTPUT_MODE_CURRENT) {
    st->out_w = w;
    st->out_h = h;
    st->have_mode = true;
  }
}
static void output_done(void *data, struct wl_output *o) { (void)data;(void)o; }
static void output_scale(void *data, struct wl_output *o, int32_t scale) {
  (void)o;
  struct mp_state *st = (struct mp_state*)data;
  if (scale < 1) scale = 1;
  st->out_scale = scale;
  st->have_scale = true;
}
static void output_name(void *data, struct wl_output *o, const char *name) { (void)data;(void)o;(void)name; }
static void output_description(void *data, struct wl_output *o, const char *desc) { (void)data;(void)o;(void)desc; }

static const struct wl_output_listener wl_output_listener = {
  .geometry = output_geometry,
  .mode = output_mode,
  .done = output_done,
  .scale = output_scale,
  .name = output_name,
  .description = output_description,
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *surf,
                            uint32_t serial, uint32_t w, uint32_t h) {
  (void)w;(void)h;
  struct mp_state *st = (struct mp_state*)data;
  zwlr_layer_surface_v1_ack_configure(surf, serial);
  st->configured = true;
}
static void layer_closed(void *data, struct zwlr_layer_surface_v1 *surf) {
  (void)surf;
  struct mp_state *st = (struct mp_state*)data;
  if (st->loop) g_main_loop_quit(st->loop);
}
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
  .configure = layer_configure,
  .closed = layer_closed,
};

// registry listener
static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *interface, uint32_t version) {
  (void)reg;
  struct mp_state *st = (struct mp_state*)data;

  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    st->compositor = wl_registry_bind(st->registry, name, &wl_compositor_interface, version > 4 ? 4 : version);
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    st->shm = wl_registry_bind(st->registry, name, &wl_shm_interface, 1);
  } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
    st->layer_shell = wl_registry_bind(st->registry, name, &zwlr_layer_shell_v1_interface, 1);
  } else if (strcmp(interface, wl_output_interface.name) == 0 && st->output == NULL) {
    // bind first output only
    uint32_t v = version > 4 ? 4 : version;
    st->output = wl_registry_bind(st->registry, name, &wl_output_interface, v);
    wl_output_add_listener(st->output, &wl_output_listener, st);
  }
}
static void registry_remove(void *data, struct wl_registry *reg, uint32_t name) {
  (void)data;(void)reg;(void)name;
}
static const struct wl_registry_listener registry_listener = {
  .global = registry_global,
  .global_remove = registry_remove,
};

// Create layer surface and shm buffers
static bool create_layer_surface(struct mp_state *st) {
  st->surface = wl_compositor_create_surface(st->compositor);
  if (!st->surface) return false;

  st->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
    st->layer_shell, st->surface, st->output,
    ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "maxpaper");
  if (!st->layer_surface) return false;

  zwlr_layer_surface_v1_set_anchor(st->layer_surface,
    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
    ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
  zwlr_layer_surface_v1_set_exclusive_zone(st->layer_surface, -1);
  zwlr_layer_surface_v1_set_keyboard_interactivity(st->layer_surface, 0);
  // Let compositor size it.
  zwlr_layer_surface_v1_set_size(st->layer_surface, 0, 0);

  zwlr_layer_surface_v1_add_listener(st->layer_surface, &layer_surface_listener, st);

  wl_surface_commit(st->surface);
  wl_display_flush(st->display);

  // Wait for configure
  for (int i=0;i<5 && !st->configured;i++) {
    wl_display_roundtrip(st->display);
  }
  return st->configured;
}

static bool create_shm_buffers(struct mp_state *st) {
  if (!st->shm) return false;

  st->buf_w = st->out_w > 0 ? st->out_w * (st->out_scale > 0 ? st->out_scale : 1) : 1920;
  st->buf_h = st->out_h > 0 ? st->out_h * (st->out_scale > 0 ? st->out_scale : 1) : 1080;
  if (st->buf_w < 16) st->buf_w = 16;
  if (st->buf_h < 16) st->buf_h = 16;

  st->stride = st->buf_w * 4;
  size_t one = (size_t)st->stride * (size_t)st->buf_h;
  st->shm_size = one * 2;

  st->shm_fd = create_shm_file(st->shm_size);
  if (st->shm_fd < 0) {
    log_err("[maxpaper] create_shm_file failed: %s", strerror(errno));
    return false;
  }
  st->shm_data = mmap(NULL, st->shm_size, PROT_READ|PROT_WRITE, MAP_SHARED, st->shm_fd, 0);
  if (st->shm_data == MAP_FAILED) {
    log_err("[maxpaper] mmap failed: %s", strerror(errno));
    close(st->shm_fd);
    st->shm_fd = -1;
    return false;
  }

  st->pool = wl_shm_create_pool(st->shm, st->shm_fd, (int)st->shm_size);
  if (!st->pool) return false;

  st->buffers[0] = wl_shm_pool_create_buffer(st->pool, 0, st->buf_w, st->buf_h, st->stride, WL_SHM_FORMAT_XRGB8888);
  st->buffers[1] = wl_shm_pool_create_buffer(st->pool, (int)one, st->buf_w, st->buf_h, st->stride, WL_SHM_FORMAT_XRGB8888);
  if (!st->buffers[0] || !st->buffers[1]) return false;

  wl_buffer_add_listener(st->buffers[0], &wl_buffer_listener, st);
  wl_buffer_add_listener(st->buffers[1], &wl_buffer_listener, st);

  st->buf_busy[0] = st->buf_busy[1] = false;
  st->cur_buf = 0;

  // Tell compositor our buffer scale
  if (st->out_scale > 0) wl_surface_set_buffer_scale(st->surface, st->out_scale);

  return true;
}

static void fill_solid(struct mp_state *st, int buf_index) {
  uint8_t *dst = (uint8_t*)st->shm_data + (size_t)buf_index * (size_t)st->stride * (size_t)st->buf_h;
  uint8_t b = (uint8_t)(st->bg_rgb & 0xFF);
  uint8_t g = (uint8_t)((st->bg_rgb >> 8) & 0xFF);
  uint8_t r = (uint8_t)((st->bg_rgb >> 16) & 0xFF);

  for (int y=0; y<st->buf_h; y++) {
    uint8_t *row = dst + (size_t)y * (size_t)st->stride;
    for (int x=0; x<st->buf_w; x++) {
      row[x*4 + 0] = b;
      row[x*4 + 1] = g;
      row[x*4 + 2] = r;
      row[x*4 + 3] = 0x00; // X byte ignored for XRGB
    }
  }
}

static void commit_buffer(struct mp_state *st, int buf_index) {
  wl_surface_attach(st->surface, st->buffers[buf_index], 0, 0);
  wl_surface_damage_buffer(st->surface, 0, 0, INT32_MAX, INT32_MAX);
  wl_surface_commit(st->surface);
  wl_display_flush(st->display);
  st->buf_busy[buf_index] = true;
}

// ---------------- GStreamer ----------------
static GstElement* make_video_filter_bin(int w, int h, int fps) {
  GstElement *bin = gst_bin_new("maxpaper_vfilter");
  GstElement *videorate = gst_element_factory_make("videorate", "vrate");
  GstElement *fps_capsf = gst_element_factory_make("capsfilter", "fpscaps");
  GstElement *videoscale = gst_element_factory_make("videoscale", "vscale");
  GstElement *videoconv = gst_element_factory_make("videoconvert", "vconv");
  GstElement *fmt_capsf = gst_element_factory_make("capsfilter", "fmtcaps");

  if (!bin || !videorate || !fps_capsf || !videoscale || !videoconv || !fmt_capsf) return NULL;

  // videorate tweaks: drop-only makes it cheaper
  g_object_set(videorate, "drop-only", TRUE, NULL);

  if (fps < 1) fps = 15;

  GstCaps *fps_caps = gst_caps_new_simple("video/x-raw",
    "framerate", GST_TYPE_FRACTION, fps, 1,
    NULL);
  g_object_set(fps_capsf, "caps", fps_caps, NULL);
  gst_caps_unref(fps_caps);

  // Cheapest possible scaling algorithm. The source is already a small,
  // heavily-compressed background video by the time it reaches us, so
  // spending CPU on high-quality interpolation just to upscale it back
  // to the panel resolution buys nothing visually and costs a lot of CPU
  // on every single frame.
  g_object_set(videoscale, "method", 0 /* nearest-neighbour */, NULL);

  // Dithering is wasted work for a video wallpaper; skip it.
  g_object_set(videoconv, "dither", 0 /* GST_VIDEO_DITHER_NONE */, NULL);

  GstCaps *fmt_caps = gst_caps_new_simple("video/x-raw",
    "format", G_TYPE_STRING, "BGRA",
    "width", G_TYPE_INT, w,
    "height", G_TYPE_INT, h,
    NULL);
  g_object_set(fmt_capsf, "caps", fmt_caps, NULL);
  gst_caps_unref(fmt_caps);

  gst_bin_add_many(GST_BIN(bin), videorate, fps_capsf, videoscale, videoconv, fmt_capsf, NULL);
  if (!gst_element_link_many(videorate, fps_capsf, videoscale, videoconv, fmt_capsf, NULL)) {
    return NULL;
  }

  // Ghost pads
  GstPad *sinkpad = gst_element_get_static_pad(videorate, "sink");
  GstPad *srcpad  = gst_element_get_static_pad(fmt_capsf, "src");
  GstPad *gsink = gst_ghost_pad_new("sink", sinkpad);
  GstPad *gsrc  = gst_ghost_pad_new("src", srcpad);
  gst_object_unref(sinkpad);
  gst_object_unref(srcpad);
  gst_element_add_pad(bin, gsink);
  gst_element_add_pad(bin, gsrc);

  return bin;
}

static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data) {
  struct mp_state *st = (struct mp_state*)user_data;
  if (!st->shm_data || st->video_failed) return GST_FLOW_OK;

  // pick a free buffer
  int next = st->cur_buf ^ 1;
  if (st->buf_busy[next]) {
    // both busy? drop frame
    if (st->buf_busy[st->cur_buf]) return GST_FLOW_OK;
    next = st->cur_buf;
  }

  GstSample *sample = gst_app_sink_pull_sample(sink);
  if (!sample) return GST_FLOW_OK;

  GstCaps *caps = gst_sample_get_caps(sample);
  GstBuffer *buffer = gst_sample_get_buffer(sample);
  if (!caps || !buffer) {
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  GstVideoInfo info;
  if (!gst_video_info_from_caps(&info, caps)) {
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  GstVideoFrame frame;
  if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  int w = GST_VIDEO_INFO_WIDTH(&info);
  int h = GST_VIDEO_INFO_HEIGHT(&info);
  if (w != st->buf_w || h != st->buf_h) {
    // Shouldn't happen due to capsfilter; handle by black fill.
    gst_video_frame_unmap(&frame);
    gst_sample_unref(sample);
    fill_solid(st, next);
    commit_buffer(st, next);
    st->cur_buf = next;
    return GST_FLOW_OK;
  }

  uint8_t *src = GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
  int src_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);

  uint8_t *dst = (uint8_t*)st->shm_data + (size_t)next * (size_t)st->stride * (size_t)st->buf_h;

  if (src_stride == st->stride) {
    // Fast path: identical row pitch, copy the whole frame in one shot
    // instead of paying per-scanline call/loop overhead at full
    // panel resolution.
    memcpy(dst, src, (size_t)st->stride * (size_t)h);
  } else {
    // Stride mismatch (alignment/padding differs) — fall back to a
    // row-by-row copy.
    for (int y=0; y<h; y++) {
      memcpy(dst + (size_t)y*(size_t)st->stride, src + (size_t)y*(size_t)src_stride, (size_t)st->stride);
    }
  }

  gst_video_frame_unmap(&frame);
  gst_sample_unref(sample);

  commit_buffer(st, next);
  st->cur_buf = next;
  return GST_FLOW_OK;
}

static gboolean on_bus_msg(GstBus *bus, GstMessage *msg, gpointer user_data) {
  (void)bus;
  struct mp_state *st = (struct mp_state*)user_data;

  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS: {
      if (st->playbin) {
        gst_element_seek_simple(st->playbin, GST_FORMAT_TIME,
          (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
      }
    } break;
    case GST_MESSAGE_ERROR: {
      GError *err = NULL;
      gchar *dbg = NULL;
      gst_message_parse_error(msg, &err, &dbg);
      log_err("[maxpaper] GStreamer error: %s", err ? err->message : "unknown");
      if (dbg) {
        // keep debug short
        if (strlen(dbg) > 400) dbg[400] = 0;
        log_err("[maxpaper] Debug: %s", dbg);
      }
      if (err) g_error_free(err);
      if (dbg) g_free(dbg);

      st->video_failed = true;
      if (st->playbin) gst_element_set_state(st->playbin, GST_STATE_NULL);
      // keep solid background; do not restart endlessly
    } break;
    default: break;
  }
  return TRUE;
}

static bool start_gstreamer(struct mp_state *st, const char *video_path) {
  if (!video_path || !video_path[0]) return false;

  char *canon = canonicalize_path(video_path);
  if (!canon || canon[0] != '/') {
    log_err("[maxpaper] VIDEO path must be absolute.");
    free(canon);
    return false;
  }
  if (access(canon, R_OK) != 0) {
    log_err("[maxpaper] VIDEO not readable: %s", canon);
    free(canon);
    return false;
  }

  // file:// uri
  char uri[PATH_MAX + 8];
  snprintf(uri, sizeof(uri), "file://%s", canon);

  st->appsink = gst_element_factory_make("appsink", "vsink");
  st->playbin = gst_element_factory_make("playbin", "pb");
  if (!st->appsink || !st->playbin) {
    log_err("[maxpaper] Missing GStreamer elements (appsink/playbin).");
    free(canon);
    return false;
  }

  // Configure appsink
  g_object_set(st->appsink,
    "emit-signals", TRUE,
    "sync", FALSE,
    "max-buffers", 2,
    "drop", TRUE,
    NULL);
  g_signal_connect(st->appsink, "new-sample", G_CALLBACK(on_new_sample), st);

  // video filter
  st->vfilter_bin = make_video_filter_bin(st->buf_w, st->buf_h, st->target_fps);
  if (!st->vfilter_bin) {
    log_err("[maxpaper] Failed to create video filter bin.");
    free(canon);
    return false;
  }

  // playbin flags: video only
  // GstPlayFlags: 0x1 video, 0x2 audio, 0x4 text, etc
  guint flags = 0x1;

  g_object_set(st->playbin,
    "uri", uri,
    "video-sink", st->appsink,
    "video-filter", st->vfilter_bin,
    "flags", flags,
    NULL);

  GstBus *bus = gst_element_get_bus(st->playbin);
  gst_bus_add_watch(bus, on_bus_msg, st);
  gst_object_unref(bus);

  GstStateChangeReturn r = gst_element_set_state(st->playbin, GST_STATE_PLAYING);
  if (r == GST_STATE_CHANGE_FAILURE) {
    log_err("[maxpaper] Failed to start playback.");
    gst_element_set_state(st->playbin, GST_STATE_NULL);
    free(canon);
    return false;
  }

  free(canon);
  return true;
}

static void stop_gstreamer(struct mp_state *st) {
  if (st->playbin) {
    gst_element_set_state(st->playbin, GST_STATE_NULL);
    gst_object_unref(st->playbin);
    st->playbin = NULL;
  }
  if (st->vfilter_bin) {
    gst_object_unref(st->vfilter_bin);
    st->vfilter_bin = NULL;
  }
  if (st->appsink) {
    gst_object_unref(st->appsink);
    st->appsink = NULL;
  }
}

// ---------------- Wayland + GLib integration ----------------
static gboolean on_wayland_io(GIOChannel *chan, GIOCondition cond, gpointer user_data) {
  (void)chan;
  struct mp_state *st = (struct mp_state*)user_data;
  if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
    if (st->loop) g_main_loop_quit(st->loop);
    return FALSE;
  }
  wl_display_dispatch(st->display);
  wl_display_flush(st->display);
  return TRUE;
}
static gboolean on_wayland_tick(gpointer user_data) {
  struct mp_state *st = (struct mp_state*)user_data;
  wl_display_dispatch_pending(st->display);
  wl_display_flush(st->display);
  return TRUE;
}

static gboolean on_sigterm(gpointer user_data) {
  struct mp_state *st = (struct mp_state*)user_data;
  if (st->loop) g_main_loop_quit(st->loop);
  return G_SOURCE_CONTINUE;
}

// ---------------- Commands ----------------
static int systemctl_user(const char *action) {
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "systemctl --user %s maxpaper.service", action);
  return system(cmd);
}

static int cmd_status(void) {
  char *cfg = config_path();
  char *video = NULL;
  char *color = NULL;
  read_kv_file(cfg, &video, &color);

  int rc = system("systemctl --user is-active --quiet maxpaper.service");
  printf("Service: %s\n", rc == 0 ? "active" : "inactive");
  printf("Video:   %s\n", (video && video[0]) ? video : "(not set)");
  printf("Color:   %s\n", (color && color[0]) ? color : "#000000");

  free(cfg); free(video); free(color);
  return 0;
}

static int cmd_setvideo(const char *path) {
  if (!path) return 2;
  if (!ensure_config_dir()) return 1;

  char *cfg = config_path();
  char *video = NULL;
  char *color = NULL;
  read_kv_file(cfg, &video, &color);

  char *canon = canonicalize_path(path);
  if (!canon || canon[0] != '/') {
    log_err("[maxpaper] setvideo expects an absolute path. Example: /home/%s/Videos/WPR.mp4", getenv("USER") ? getenv("USER") : "user");
    free(cfg); free(video); free(color); free(canon);
    return 2;
  }

  free(video);
  video = canon;

  if (!color) color = strdup("#000000");
  write_kv_file(cfg, video, color);

  printf("Saved VIDEO=%s\n", video);

  // If service is active, restart for instant apply
  if (system("systemctl --user is-active --quiet maxpaper.service") == 0) {
    systemctl_user("restart");
  }

  free(cfg); free(video); free(color);
  return 0;
}

static int cmd_start(void)   { systemctl_user("enable --now"); return 0; }
static int cmd_stop(void)    { systemctl_user("disable --now"); return 0; }
static int cmd_restart(void) { systemctl_user("restart"); return 0; }

// ---------------- Run (service) ----------------
static int cmd_run(void) {
  struct mp_state st;
  memset(&st, 0, sizeof(st));
  st.out_scale = 1;

  // Display-side frame cap. This is independent of MAXPAPER_OPT_FPS
  // (which only affects the one-time ffmpeg encode in `setvideo`/`optimize`).
  // 15fps is plenty smooth for an ambient background video and roughly
  // halves the per-second cost of scale+convert+copy+commit compared to
  // the old hardcoded 30fps cap, which was a major source of the reported
  // lag on weaker hardware.
  st.target_fps = 15;
  const char *fps_env = getenv("MAXPAPER_RENDER_FPS");
  if (fps_env && fps_env[0]) {
    int v = atoi(fps_env);
    if (v >= 1 && v <= 60) st.target_fps = v;
  }

  // Read config
  char *cfg = config_path();
  char *video = NULL;
  char *color = NULL;
  read_kv_file(cfg, &video, &color);
  free(cfg);

  st.video_path = video;
  st.bg_rgb = parse_color_rgb(color ? color : "#000000");
  free(color);

  // Init Wayland
  st.display = wl_display_connect(NULL);
  if (!st.display) {
    log_err("[maxpaper] wl_display_connect failed.");
    free(st.video_path);
    return 1;
  }

  st.registry = wl_display_get_registry(st.display);
  wl_registry_add_listener(st.registry, &registry_listener, &st);
  wl_display_roundtrip(st.display);
  wl_display_roundtrip(st.display);

  if (!st.compositor || !st.layer_shell || !st.shm) {
    log_err("[maxpaper] Missing Wayland globals (compositor=%p layer_shell=%p shm=%p).", (void*)st.compositor, (void*)st.layer_shell, (void*)st.shm);
    wl_display_disconnect(st.display);
    free(st.video_path);
    return 1;
  }

  if (!st.have_mode) {
    // fallback
    st.out_w = 1920; st.out_h = 1080;
  }
  if (!st.have_scale) st.out_scale = 1;

  if (!create_layer_surface(&st)) {
    log_err("[maxpaper] Failed to create layer surface (no configure).");
    wl_display_disconnect(st.display);
    free(st.video_path);
    return 1;
  }

  if (!create_shm_buffers(&st)) {
    log_err("[maxpaper] Failed to create shm buffers.");
    wl_display_disconnect(st.display);
    free(st.video_path);
    return 1;
  }

  // Show initial solid background
  fill_solid(&st, 0);
  commit_buffer(&st, 0);
  st.cur_buf = 0;

  // Init GStreamer
  int argc = 0; char **argv = NULL;
  gst_init(&argc, &argv);

  if (st.video_path && st.video_path[0]) {
    start_gstreamer(&st, st.video_path);
  }

  // Main loop
  st.loop = g_main_loop_new(NULL, FALSE);

  int fd = wl_display_get_fd(st.display);
  GIOChannel *ch = g_io_channel_unix_new(fd);
  g_io_add_watch(ch, (GIOCondition)(G_IO_IN | G_IO_ERR | G_IO_HUP), on_wayland_io, &st);
  // This is just a safety-net poll for any Wayland event that might not
  // trigger the fd watch above; actual frame delivery is driven by the
  // appsink callback. 200ms (5Hz) instead of the old 33ms (30Hz) cuts
  // ~85% of these wakeups, which matters for idle CPU usage/power on
  // weaker laptop hardware.
  g_timeout_add(200, on_wayland_tick, &st);

  // SIGTERM/SIGINT
#if GLIB_CHECK_VERSION(2,36,0)
  g_unix_signal_add(SIGINT, on_sigterm, &st);
  g_unix_signal_add(SIGTERM, on_sigterm, &st);
#endif

  g_main_loop_run(st.loop);

  // Cleanup
  stop_gstreamer(&st);

  if (st.pool) wl_shm_pool_destroy(st.pool);
  if (st.buffers[0]) wl_buffer_destroy(st.buffers[0]);
  if (st.buffers[1]) wl_buffer_destroy(st.buffers[1]);
  if (st.shm_data && st.shm_data != MAP_FAILED) munmap(st.shm_data, st.shm_size);
  if (st.shm_fd >= 0) close(st.shm_fd);

  if (st.layer_surface) zwlr_layer_surface_v1_destroy(st.layer_surface);
  if (st.surface) wl_surface_destroy(st.surface);

  if (st.output) wl_output_destroy(st.output);
  if (st.layer_shell) zwlr_layer_shell_v1_destroy(st.layer_shell);
  if (st.shm) wl_shm_destroy(st.shm);
  if (st.compositor) wl_compositor_destroy(st.compositor);
  if (st.registry) wl_registry_destroy(st.registry);

  if (st.loop) g_main_loop_unref(st.loop);

  wl_display_disconnect(st.display);
  free(st.video_path);
  return 0;
}

// ---------------- main ----------------
static void usage(void) {
  printf("maxpaper commands:\n");
  printf("  maxpaper setvideo /absolute/path/to/video.mp4\n");
  printf("  maxpaper start\n");
  printf("  maxpaper stop\n");
  printf("  maxpaper restart\n");
  printf("  maxpaper status\n");
}

int main(int argc, char **argv) {
  if (argc < 2) { usage(); return 2; }

  if (strcmp(argv[1], "run") == 0) {
    return cmd_run();
  } else if (strcmp(argv[1], "setvideo") == 0) {
    return cmd_setvideo(argc >= 3 ? argv[2] : NULL);
  } else if (strcmp(argv[1], "start") == 0) {
    return cmd_start();
  } else if (strcmp(argv[1], "stop") == 0) {
    return cmd_stop();
  } else if (strcmp(argv[1], "restart") == 0) {
    return cmd_restart();
  } else if (strcmp(argv[1], "status") == 0) {
    return cmd_status();
  }

  usage();
  return 2;
}
