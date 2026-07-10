#!/usr/bin/env bash
# Cross-compile einkbridge (aarch64, glibc).
#
# einkbridge links against the vendor's Qt 6.8.2 runtime at run time
# (bundle libs, /opt/eink/lib on the device). It is compiled against newer
# Qt6 headers with --allow-shlib-undefined; avoid APIs newer than 6.8
# (e.g. QObject::setProperty inlines to a 6.9+ symbol in newer headers —
# use QQmlProperty::write instead, as the source does).
#
# Required environment:
#   CXX          aarch64-linux-gnu C++ cross compiler
#                  (default: aarch64-linux-gnu-g++ from PATH)
#   QT_BASE      Qt6 base install with include/QtCore, include/QtGui
#                  (aarch64 headers preferred; QtCore/QtGui are mostly
#                   arch-neutral apart from qconfig)
#   QT_QML_HDRS  Qt6 declarative include dir with QtQuick/, QtQml/
#                  (arch-neutral — an x86_64 qtdeclarative works)
#
# The vendor bundle must already exist at eink/bundle/eink/lib (built by
# build-bundle.sh) — it provides the link-time .so stubs.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
SRC=$REPO/eink/src
OUT=$REPO/eink/bin/einkbridge
BUNDLE=$REPO/eink/bundle/eink/lib

CXX=${CXX:-aarch64-linux-gnu-g++}
: "${QT_BASE:?set QT_BASE to a Qt6 install prefix (with include/QtCore etc.)}"
: "${QT_QML_HDRS:?set QT_QML_HDRS to a Qt6 declarative include dir (with QtQuick/)}"

command -v "$CXX" >/dev/null || { echo "cross compiler not found: $CXX"; exit 1; }
[ -d "$BUNDLE" ] || { echo "vendor bundle missing at $BUNDLE — run build-bundle.sh first"; exit 1; }

mkdir -p "$(dirname "$OUT")"

echo "==> compile + link"
"$CXX" -O2 -fPIE -std=c++17 \
    -I"$QT_BASE/include" \
    -I"$QT_BASE/include/QtCore" \
    -I"$QT_BASE/include/QtGui" \
    -I"$QT_QML_HDRS" \
    -I"$QT_QML_HDRS/QtQuick" \
    -I"$QT_QML_HDRS/QtQml" \
    -L"$BUNDLE" \
    -Wl,--allow-shlib-undefined \
    "$SRC/einkbridge.cpp" \
    -lQt6Core -lQt6Gui -lQt6Quick -lQt6Qml \
    -o "$OUT"

echo "==> patchelf (interpreter + rpath -> /opt/eink/lib)"
patchelf --set-interpreter /opt/eink/lib/ld-linux-aarch64.so.1 \
         --set-rpath '/opt/eink/lib' "$OUT"

echo "==> done: $OUT"
file "$OUT"
