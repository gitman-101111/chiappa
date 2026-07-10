#!/usr/bin/env bash
# Assemble the self-contained glibc/Qt6 e-ink runtime bundle (for musl-based distros).
# Output: eink/bundle/eink/  -> deploy to /opt/eink on the device.
# Needs: /tmp/closure.txt (from closure.py), vendor rootfs at $VROOT, patchelf.
set -euo pipefail
VROOT=${VROOT:-/mnt/vroot}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$REPO/eink/bundle/eink
PANEL_LOT=${PANEL_LOT:-AAB0B7}          # this device's EPD lot (GAL3_AAB0B7_*)

rm -rf "$OUT"
mkdir -p "$OUT"/{lib,plugins/platforms,plugins/scenegraph,plugins/imageformats,qml,fonts,waveforms,bin}

echo "== 1. closure libs + soname symlinks =="
while read -r real _; do
  [ -f "$real" ] || continue
  cp -a "$real" "$OUT/lib/"
  b=$(basename "$real"); d=$(dirname "$real")
  # recreate every symlink in the source dir that points at this realfile
  for l in "$d"/*; do
    [ -L "$l" ] || continue
    tgt=$(readlink "$l")
    if [ "$(basename "$tgt")" = "$b" ]; then
      ln -sf "$b" "$OUT/lib/$(basename "$l")"
    fi
  done
done < <(grep '^/' /tmp/closure.txt)
# guarantee the canonical Qt6 .so.6 sonames exist (defensive)
for f in "$OUT"/lib/libQt6*.so.6.8.2; do
  [ -e "$f" ] || continue
  base=$(basename "$f"); so6=${base%.6.8.2}.6
  [ -e "$OUT/lib/$so6" ] || ln -sf "$base" "$OUT/lib/$so6"
done

echo "== 2. plugins =="
cp -a "$VROOT/usr/lib/plugins/platforms/libepaper.so"     "$OUT/plugins/platforms/"
cp -a "$VROOT/usr/lib/plugins/scenegraph/libqsgepaper.so" "$OUT/plugins/scenegraph/"
cp -a "$VROOT"/usr/lib/plugins/imageformats/*.so          "$OUT/plugins/imageformats/"

echo "== 3. QML modules (scene.qml uses QtQuick -> pulls QtQml/QtCore) =="
for m in QtQuick QtQml QtCore; do
  cp -a "$VROOT/usr/lib/qml/$m" "$OUT/qml/"
done

echo "== 4. fonts =="
cp -a "$VROOT/usr/share/fonts/ttf" "$OUT/fonts/" 2>/dev/null || \
  cp -a "$VROOT"/usr/share/fonts/* "$OUT/fonts/"

echo "== 5. waveforms (this panel's lot + colortables) =="
cp -a "$VROOT"/usr/share/remarkable/GAL3_${PANEL_LOT}_*.eink "$OUT/waveforms/"
cp -a "$VROOT"/usr/share/remarkable/colortable_*.bin "$OUT/waveforms/"
cp -a "$VROOT"/usr/share/remarkable/ct33_*.bin        "$OUT/waveforms/"

echo "== 6. app + scene =="
cp "$REPO/eink/bin/qquick_m3" "$OUT/bin/"
cp "$REPO/eink/scene.qml"     "$OUT/"

echo "== 7. patchelf app -> bundled loader + rpath =="
patchelf --set-interpreter /opt/eink/lib/ld-linux-aarch64.so.1 \
         --set-rpath '/opt/eink/lib' "$OUT/bin/qquick_m3"

echo "== 8. run.sh =="
cat > "$OUT/run.sh" <<'EOF'
#!/bin/sh
# Run the e-ink engine from the bundle (self-contained glibc/Qt6).
HERE=$(cd "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH="$HERE/lib"
export QT_PLUGIN_PATH="$HERE/plugins"
export QML2_IMPORT_PATH="$HERE/qml"
export QML_IMPORT_PATH="$HERE/qml"
export QT_QPA_PLATFORM_PLUGIN_PATH="$HERE/plugins/platforms"
export QT_QPA_FONTDIR="$HERE/fonts"
export QT_QUICK_BACKEND=epaper
export QT_QPA_PLATFORM=epaper
# waveforms: engine reads /usr/share/remarkable by default; override if supported
export RM_WAVEFORM_DIR="$HERE/waveforms"
exec "$HERE/lib/ld-linux-aarch64.so.1" "$HERE/bin/qquick_m3" "${1:-$HERE/scene.qml}"
EOF
chmod +x "$OUT/run.sh"

echo "== done =="
du -sh "$OUT"
find "$OUT" -maxdepth 2 -type d | sort
