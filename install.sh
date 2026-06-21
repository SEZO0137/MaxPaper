#!/usr/bin/env bash
set -euo pipefail

PREFIX="/usr/local"
BINDIR="$PREFIX/bin"
LIBEXECDIR="$PREFIX/libexec/maxpaper"
CLI_BIN="$BINDIR/maxpaper"
CORE_BIN="$LIBEXECDIR/maxpaper-bin"

echo "[maxpaper] Installing build/runtime dependencies..."
sudo apt update -y
sudo apt install -y \
  build-essential pkg-config ffmpeg \
  libwayland-dev libwayland-bin \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav

echo "[maxpaper] Building..."
make clean >/dev/null 2>&1 || true
make

echo "[maxpaper] Installing binaries..."
sudo install -d "$BINDIR" "$LIBEXECDIR"
sudo install -m 0755 ./maxpaper "$CORE_BIN"
sudo install -m 0755 ./maxpaper-wrapper "$CLI_BIN"

echo "[maxpaper] Installing user systemd unit..."
mkdir -p "$HOME/.config/systemd/user"
install -m 0644 ./maxpaper.service "$HOME/.config/systemd/user/maxpaper.service"

echo "[maxpaper] Creating default config (if missing)..."
mkdir -p "$HOME/.config/maxpaper"
if [[ ! -f "$HOME/.config/maxpaper/config" ]]; then
  cat >"$HOME/.config/maxpaper/config" <<CFG
# MaxPaper config
VIDEO=
SOURCE_VIDEO=
COLOR=#000000
CFG
fi

echo "[maxpaper] Reloading user systemd..."
systemctl --user daemon-reload

echo "[maxpaper] Enabling + starting service..."
systemctl --user enable --now maxpaper.service || true

echo
echo "[maxpaper] Done."
echo "  Set your video: maxpaper setvideo \"/home/$USER/Videos/WPR.mp4\""
echo "  Restart:        maxpaper restart"
echo "  Status:         maxpaper status"
echo "  Uninstall:      maxpaper uninstall   (or ./uninstall.sh from this folder)"
echo "  Tip:            setvideo now creates an ffmpeg-optimized cache for smoother playback"
