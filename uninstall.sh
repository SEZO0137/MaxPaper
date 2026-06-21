#!/usr/bin/env bash
set -euo pipefail

PREFIX="/usr/local"
BINDIR="$PREFIX/bin"
LIBEXECDIR="$PREFIX/libexec/maxpaper"
CLI_BIN="$BINDIR/maxpaper"
CORE_BIN="$LIBEXECDIR/maxpaper-bin"

echo "[maxpaper] Stopping + disabling user service (if present)..."
systemctl --user disable --now maxpaper.service 2>/dev/null || true

echo "[maxpaper] Removing unit + config/cache..."
rm -f "$HOME/.config/systemd/user/maxpaper.service"
rm -rf "$HOME/.config/maxpaper"
rm -rf "${XDG_CACHE_HOME:-$HOME/.cache}/maxpaper"

echo "[maxpaper] Reloading user systemd..."
systemctl --user daemon-reload

echo "[maxpaper] Removing binaries..."
sudo rm -f "$CLI_BIN" "$CORE_BIN"
sudo rmdir "$LIBEXECDIR" 2>/dev/null || true

echo "[maxpaper] Uninstalled."
