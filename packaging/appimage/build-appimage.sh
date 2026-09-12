#!/bin/sh
# Build a portable AppImage of the Linux port.
#
# Stages a Freedesktop-standard AppDir from the CMake install target, bundles
# the shared libraries that are not safe to expect from an arbitrary host, then
# turns it into a single-file AppImage with appimagetool.
#
# Usage (from the repository root):
#
#   sh packaging/appimage/build-appimage.sh
#
# Environment:
#   BUILD_DIR      CMake build directory (default: build/appimage)
#   APPDIR         staging directory     (default: build/appimage/Ultima3.AppDir)
#   OUTPUT         AppImage path         (default: build/Ultima3-x86_64.AppImage)
#   APPIMAGETOOL   appimagetool binary   (default: found on $PATH)
#   PYTHON         interpreter with Pillow, for the asset export
#                                        (default: python3)
#
# Without appimagetool the AppDir is still produced and validated, and the
# command to finish the job is printed -- so this is useful even on a machine
# that has no appimagetool installed.
set -eu

BUILD_DIR="${BUILD_DIR:-build/appimage}"
APPDIR="${APPDIR:-$BUILD_DIR/Ultima3.AppDir}"
OUTPUT="${OUTPUT:-build/Ultima3-x86_64.AppImage}"
PYTHON="${PYTHON:-python3}"

if [ ! -f CMakeLists.txt ]; then
    echo "error: run this from the repository root." >&2
    exit 1
fi

if ! "$PYTHON" -c 'from PIL import Image' 2>/dev/null; then
    echo "error: \$PYTHON ($PYTHON) cannot import Pillow, which the asset export needs." >&2
    echo "       Install python3-pil, or set PYTHON to an interpreter that has it." >&2
    exit 1
fi

echo "==> Configuring and building in $BUILD_DIR"
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DU3_BUILD_GAME_CORE=ON \
    -DU3_SANITIZERS=OFF \
    -DPython3_EXECUTABLE="$(command -v "$PYTHON")"
cmake --build "$BUILD_DIR" --parallel

echo "==> Staging $APPDIR"
rm -rf "$APPDIR"
cmake --install "$BUILD_DIR" --prefix "$APPDIR/usr" >/dev/null

# AppImage expects the entry point, desktop entry and icon at the AppDir root.
install -m 0755 packaging/appimage/AppRun "$APPDIR/AppRun"
install -m 0644 packaging/ultima3.desktop "$APPDIR/ultima3.desktop"
install -m 0644 packaging/icons/hicolor/256x256/apps/ultima3.png "$APPDIR/ultima3.png"
cp "$APPDIR/ultima3.png" "$APPDIR/.DirIcon"

echo "==> Bundling libraries"
mkdir -p "$APPDIR/usr/lib"
# Bundle the game's own stack (SDL2, FluidSynth, JSON-C and what they pull in),
# but never the pieces that must match the host: glibc and its loader, the
# graphics and input drivers, and the X11/Wayland client libraries. Bundling
# those is the classic way to make an AppImage that runs only on the machine
# that built it.
ldd "$APPDIR/usr/bin/ultima3" | awk '/=> \//{print $3}' | sort -u | while read -r lib; do
    case "$(basename "$lib")" in
        libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|ld-linux*.so.*) continue ;;
        libstdc++.so.*|libgcc_s.so.*) continue ;;
        libGL*.so.*|libEGL*.so.*|libGLX*.so.*|libgbm.so.*|libdrm.so.*) continue ;;
        libX11*.so.*|libxcb*.so.*|libXext.so.*|libXrandr.so.*|libXi.so.*|libXfixes.so.*) continue ;;
        libwayland-*.so.*) continue ;;
    esac
    cp -L "$lib" "$APPDIR/usr/lib/"
done

echo "==> Validating the AppDir"
for required in AppRun ultima3.desktop ultima3.png usr/bin/ultima3 \
                usr/share/ultima3/assets/manifest.json; do
    if [ ! -e "$APPDIR/$required" ]; then
        echo "error: staged AppDir is missing $required" >&2
        exit 1
    fi
done
if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$APPDIR/ultima3.desktop"
fi

APPIMAGETOOL="${APPIMAGETOOL:-$(command -v appimagetool 2>/dev/null || true)}"
if [ -z "$APPIMAGETOOL" ]; then
    cat <<EOF

AppDir staged and validated: $APPDIR

appimagetool was not found, so no single-file AppImage was produced. Install it
from https://github.com/AppImage/AppImageKit/releases and then run:

    appimagetool "$APPDIR" "$OUTPUT"

The AppDir is already runnable as-is:

    "$APPDIR/AppRun"
EOF
    exit 0
fi

echo "==> Packaging $OUTPUT"
mkdir -p "$(dirname "$OUTPUT")"
"$APPIMAGETOOL" "$APPDIR" "$OUTPUT"
echo "Wrote $OUTPUT"
