#!/bin/sh
# package.sh -- a release folder and archive for one system, from a finished build.
#
#   tools/package.sh macos|linux|windows BUILD_DIR
#
# Writes dist/blmemu-VERSION-SYSTEM/ and its .zip (macOS, Windows) or .tar.gz (Linux).  No firmware goes in.
set -e
cd "$(dirname "$0")/.."
SYS=$1
BUILD=$2
[ -n "$SYS" ] && [ -d "$BUILD" ] || { echo "usage: tools/package.sh macos|linux|windows BUILD_DIR" >&2; exit 2; }
VER=$(sed -n 's/^project(blmemu VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
ARCH=$(uname -m)
case $SYS in
    macos) NAME=blmemu-$VER-macos ;;
    linux) NAME=blmemu-$VER-linux-$ARCH ;;
    windows) NAME=blmemu-$VER-windows-x64 ;;
    *) echo "unknown system $SYS" >&2; exit 2 ;;
esac
OUT=dist/$NAME
rm -rf "$OUT"
mkdir -p "$OUT/licenses"
cp licenses/* "$OUT/licenses/"

case $SYS in
windows)
    cp "$BUILD/blmemu.exe" "$OUT/"
    # Notepad-friendly copies of the texts
    sed 's/$/\r/' README.md > "$OUT/README.txt"
    sed 's/$/\r/' LICENSE > "$OUT/LICENSE.txt"
    ;;
macos)
    cp "$BUILD/blmemu" "$OUT/"
    codesign --force -s - "$OUT/blmemu" 2>/dev/null || true      # an ad-hoc signature: Apple silicon needs one
    cp README.md LICENSE "$OUT/"
    cat > "$OUT/Braille Lite Millennium.command" <<'EOF'
#!/bin/sh
# Double-click: the emulator in a Terminal window.
cd "$(dirname "$0")" && exec ./blmemu
EOF
    chmod +x "$OUT/Braille Lite Millennium.command"
    ;;
linux)
    cp "$BUILD/blmemu" "$OUT/"
    strip "$OUT/blmemu" 2>/dev/null || true
    cp README.md LICENSE "$OUT/"
    ;;
esac

cd dist
rm -f "$NAME.zip" "$NAME.tar.gz"
case $SYS in
    linux) tar -czf "$NAME.tar.gz" "$NAME" && echo "dist/$NAME.tar.gz" ;;
    *) if command -v zip >/dev/null; then zip -qr "$NAME.zip" "$NAME"; else 7z a -bd "$NAME.zip" "$NAME" >/dev/null; fi
       echo "dist/$NAME.zip" ;;
esac
