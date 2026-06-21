# MaxPaper

A lightweight, single-video live wallpaper engine for **Wayland** (Pop!\_OS / COSMIC, and any `wlr-layer-shell` compositor). MaxPaper plays one looping video as your desktop background using GStreamer for decoding and `wlr-layer-shell` for placement — no compositor-specific hacks required.

## v19 — Performance fix

If you came from v18 and the wallpaper was making your whole machine feel sluggish, this release is for you. The root cause was a default mismatch: the optimizer was silently encoding wallpaper video at **40 fps** even though it was documented (and intended) to use 12 fps — over 3x more decode/scale/composite work, every second, forever, in the background.

**What changed in v19:**

- Fixed the `setvideo` optimizer default back down to **12 fps** (was incorrectly defaulting to 40).
- Added a separate, configurable **display-side frame cap** (`MAXPAPER_RENDER_FPS`, default `15`) so the renderer itself never pushes more full-screen composites per second than it needs to, regardless of the source video's frame rate.
- Switched the upscale step to the cheapest scaling algorithm (nearest-neighbour) instead of a quality filter that bought nothing visually on an already-compressed background video.
- Cut a redundant idle polling timer from 30 wakeups/sec down to 5/sec.
- Optimized the per-frame buffer copy to a single `memcpy` in the common case instead of a per-scanline loop.
- The systemd service now runs at **lower CPU/IO priority** (`Nice`, `IOSchedulingClass`, `CPUWeight`) so the wallpaper can never win a resource fight against whatever you're actually doing.
- Added a built-in `maxpaper uninstall` command — no need to keep the source folder around just to remove it.

Net effect: noticeably lower CPU usage, no more system-wide lag from a background wallpaper, and the video still loops smoothly.

## Features

- Single static video looped as your desktop background, rendered via Wayland layer-shell (`background` layer, behind everything).
- Automatic ffmpeg-based optimization pass: strips audio, downscales, and re-encodes to a small, decode-friendly H.264 file before it ever touches the renderer.
- Runs as a `systemd --user` service — starts with your session, restarts on failure, fully controllable from the CLI.
- Tunable at every stage: encode resolution/fps/quality, and display-side frame cap, all via simple environment variables.
- Small, dependency-light C core (GLib + GStreamer + `libwayland-client`); no Electron, no browser engine.

## Requirements

- A Wayland compositor that implements `wlr-layer-shell-unstable-v1` (e.g. COSMIC, Sway, Hyprland).
- `ffmpeg`, GStreamer 1.0 (+ `app` and `video` plugins), and standard Wayland client dev libraries — `install.sh` installs all of this for you on Debian/Ubuntu-based systems (including Pop!\_OS).

## Install

```bash
git clone https://github.com/<your-username>/maxpaper.git
cd maxpaper
./install.sh
```

`install.sh` installs build/runtime dependencies, builds the renderer, installs the CLI to `/usr/local/bin/maxpaper`, installs a `systemd --user` unit, and enables + starts the service.

## Usage

```bash
maxpaper setvideo "/home/$USER/Videos/WPR.mp4"   # set + optimize + apply a video
maxpaper status                                   # show current state
maxpaper restart                                   # reload after changes
```

## Commands

| Command | Description |
|---|---|
| `maxpaper setvideo /abs/path/to/video.mp4` | Set the wallpaper source, run it through the ffmpeg optimizer, save it, and (re)start the service. |
| `maxpaper optimize /abs/path/to/video.mp4` | Re-run just the optimization step (e.g. after changing `MAXPAPER_OPT_*` env vars). |
| `maxpaper start` | Enable + start the service. |
| `maxpaper stop` | Stop + disable the service. |
| `maxpaper restart` | Restart the service (also done automatically by `setvideo`). |
| `maxpaper status` | Print service state, video path, and color. |
| `maxpaper uninstall` | Remove the service, config, cache, and installed binaries. |

## Configuration

| File | Purpose |
|---|---|
| `~/.config/maxpaper/config` | `VIDEO`, `SOURCE_VIDEO`, `COLOR` (fallback background color). |
| `~/.cache/maxpaper/optimized.mp4` | The ffmpeg-optimized video actually played by the renderer. |
| `~/.config/systemd/user/maxpaper.service` | The user service unit. |

### Tuning the one-time video encode

Set these **before** running `setvideo` or `optimize`:

```bash
export MAXPAPER_OPT_WIDTH=480    # max output width in px
export MAXPAPER_OPT_FPS=12       # output frame rate
export MAXPAPER_OPT_CRF=38       # higher = smaller/blurrier, lighter to decode
export MAXPAPER_OPT_PRESET=medium
maxpaper setvideo "/home/$USER/Videos/WPR.mp4"
```

Lower width / higher CRF / lower fps = lighter playback.

### Tuning the live renderer

Set in the systemd unit (`~/.config/systemd/user/maxpaper.service`) or your shell environment before starting the service:

```ini
Environment=MAXPAPER_RENDER_FPS=15
```

This caps how many times per second the renderer scales, converts, and composites a frame — independent of the source video's own frame rate. Lower it (e.g. `10`) on weaker hardware; raise it (up to `60`) if you have CPU to spare and want smoother motion.

After editing the unit file:

```bash
systemctl --user daemon-reload
maxpaper restart
```

## Troubleshooting

- **Nothing shows up / wrong screen:** your compositor must support `wlr-layer-shell-unstable-v1`. Check compositor logs/docs if unsure.
- **Logs:** `journalctl --user -u maxpaper.service -f`
- **Still feels heavy:** lower `MAXPAPER_RENDER_FPS`, lower `MAXPAPER_OPT_WIDTH`/raise `MAXPAPER_OPT_CRF`, then `maxpaper optimize <source>` to re-encode.

## Uninstall

```bash
maxpaper uninstall
```

or, if you still have the source folder:

```bash
cd maxpaper
./uninstall.sh
```

Both remove the service, config, cache, and installed binaries.

## License

MIT — see [LICENSE](LICENSE).

## Contributing

Issues and pull requests are welcome. If you hit a compositor that doesn't behave, please include `journalctl --user -u maxpaper.service` output and your compositor name/version.
