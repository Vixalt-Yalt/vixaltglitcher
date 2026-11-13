#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

VERSION="${VERSION:-1.0.0}"
ARCH="x86_64"
APPDIR="${ROOT_DIR}/AppDir"
OUTPUT="${OUTPUT:-${ROOT_DIR}/dist/VIXGlitcher-${ARCH}.AppImage}"

echo "==> [1/4] Building VIXGlitcher v${VERSION} (${ARCH})..."
if [[ -d "/tmp/deps/SDL-release-2.32.10/include" && -f "/tmp/sdl2_lib/libSDL2-2.0.so.0" ]]; then
  make clean
  make SDL2_CFLAGS="-I/tmp/deps/SDL-release-2.32.10/include" \
       SDL2_LIBS="-L/tmp/sdl2_lib -lSDL2"
else
  make clean
  make
fi

strip --strip-unneeded build/vixglitcher

echo "==> [2/4] Staging AppDir..."
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib"
make PREFIX=/usr DESTDIR="$APPDIR" install

install -Dm755 packaging/AppRun "$APPDIR/AppRun"
install -Dm644 packaging/vixglitcher.desktop "$APPDIR/vixglitcher.desktop"
install -Dm644 assets/icons/vixglitcher-256.png "$APPDIR/vixglitcher.png"
ln -sf vixglitcher.png "$APPDIR/.DirIcon"

# Bundle portable SDL2 (Wayland + X11 + KMSDRM dynamic backend, glibc-only dependencies)
if [[ -f "/tmp/sdl2_lib/libSDL2-2.0.so.0" ]]; then
  install -Dm755 "/tmp/sdl2_lib/libSDL2-2.0.so.0" "$APPDIR/usr/lib/libSDL2-2.0.so.0"
elif [[ -f "/usr/lib/libSDL2-2.0.so.0" ]]; then
  install -Dm755 "/usr/lib/libSDL2-2.0.so.0" "$APPDIR/usr/lib/libSDL2-2.0.so.0"
elif [[ -f "/usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0" ]]; then
  install -Dm755 "/usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0" "$APPDIR/usr/lib/libSDL2-2.0.so.0"
fi

echo "==> [3/4] Packing SquashFS + AppImage Type-2 runtime..."
mkdir -p "$(dirname "$OUTPUT")"

MKSQUASHFS_BIN="${MKSQUASHFS_BIN:-}"
if [[ -z "$MKSQUASHFS_BIN" ]]; then
  if [[ -x "/tmp/appimage-toolchain/prefix/bin/mksquashfs" ]]; then
    MKSQUASHFS_BIN="/tmp/appimage-toolchain/prefix/bin/mksquashfs"
  elif command -v mksquashfs >/dev/null 2>&1; then
    MKSQUASHFS_BIN="$(command -v mksquashfs)"
  else
    echo "Error: mksquashfs not found" >&2
    exit 1
  fi
fi

RUNTIME_BIN="${RUNTIME_BIN:-}"
if [[ -z "$RUNTIME_BIN" ]]; then
  if [[ -x "/tmp/appimage-toolchain/prefix/bin/runtime-x86_64" ]]; then
    RUNTIME_BIN="/tmp/appimage-toolchain/prefix/bin/runtime-x86_64"
  elif [[ -f "${ROOT_DIR}/build/runtime-x86_64" ]]; then
    RUNTIME_BIN="${ROOT_DIR}/build/runtime-x86_64"
  fi
fi

if [[ -n "$RUNTIME_BIN" && -f "$RUNTIME_BIN" ]]; then
  SQFS_TMP="$(mktemp /tmp/vixglitcher-XXXXXX.sqfs)"
  rm -f "$SQFS_TMP"
  "$MKSQUASHFS_BIN" "$APPDIR" "$SQFS_TMP" -root-owned -noappend -comp zstd -Xcompression-level 19 -no-progress >/dev/null
  cat "$RUNTIME_BIN" "$SQFS_TMP" > "$OUTPUT"
  chmod +x "$OUTPUT"
  rm -f "$SQFS_TMP"
elif command -v appimagetool >/dev/null 2>&1; then
  ARCH="$ARCH" appimagetool "$APPDIR" "$OUTPUT"
else
  echo "Error: Neither runtime-x86_64 nor appimagetool found" >&2
  exit 1
fi

(cd "$(dirname "$OUTPUT")" && sha256sum "$(basename "$OUTPUT")" > "$(basename "$OUTPUT").sha256")
rm -rf "$APPDIR"

echo "==> [4/4] Verifying AppImage..."
ls -lh "$OUTPUT" "${OUTPUT}.sha256"
"$OUTPUT" --appimage-extract-and-run --version
echo "==> Done: $OUTPUT"
