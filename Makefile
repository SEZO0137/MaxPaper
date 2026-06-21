PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

CC ?= cc
CFLAGS ?= -O2 -pipe -Wall -Wextra -Wno-unused-parameter -DWLR_USE_UNSTABLE
PKG_CONFIG ?= pkg-config

GST_CFLAGS := $(shell $(PKG_CONFIG) --cflags gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 glib-2.0)
GST_LIBS   := $(shell $(PKG_CONFIG) --libs   gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 glib-2.0)

WAYLAND_LIBS := -lwayland-client

PROTO_DIR = protocol
BUILD_DIR = build
PROTO_BUILD = $(BUILD_DIR)/protocol

LAYER_XML = $(PROTO_DIR)/wlr-layer-shell-unstable-v1.xml
XDG_XML   = $(PROTO_DIR)/xdg-shell.xml

LAYER_HDR = $(PROTO_BUILD)/wlr-layer-shell-unstable-v1-client-protocol.h
LAYER_SRC = $(PROTO_BUILD)/wlr-layer-shell-unstable-v1-client-protocol.c
XDG_HDR   = $(PROTO_BUILD)/xdg-shell-client-protocol.h
XDG_SRC   = $(PROTO_BUILD)/xdg-shell-client-protocol.c

PROTO_OBJ = $(PROTO_BUILD)/wlr-layer-shell.o $(PROTO_BUILD)/xdg-shell.o

SRC = src/maxpaper.c
BIN = maxpaper

all: $(BIN)

$(PROTO_BUILD):
	mkdir -p $(PROTO_BUILD)

$(LAYER_HDR): $(LAYER_XML) | $(PROTO_BUILD)
	wayland-scanner client-header $< $@

$(LAYER_SRC): $(LAYER_XML) | $(PROTO_BUILD)
	wayland-scanner private-code $< $@

$(XDG_HDR): $(XDG_XML) | $(PROTO_BUILD)
	wayland-scanner client-header $< $@

$(XDG_SRC): $(XDG_XML) | $(PROTO_BUILD)
	wayland-scanner private-code $< $@

$(PROTO_BUILD)/wlr-layer-shell.o: $(LAYER_SRC) $(LAYER_HDR) $(XDG_HDR)
	$(CC) $(CFLAGS) $(GST_CFLAGS) -c $(LAYER_SRC) -o $@

$(PROTO_BUILD)/xdg-shell.o: $(XDG_SRC) $(XDG_HDR)
	$(CC) $(CFLAGS) $(GST_CFLAGS) -c $(XDG_SRC) -o $@

$(BIN): $(SRC) $(PROTO_OBJ) $(LAYER_HDR) $(XDG_HDR)
	$(CC) $(CFLAGS) $(GST_CFLAGS) -I$(PROTO_BUILD) $(SRC) $(PROTO_OBJ) $(WAYLAND_LIBS) $(GST_LIBS) -o $@

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/maxpaper

clean:
	rm -rf $(BUILD_DIR) $(BIN)

.PHONY: all install clean
