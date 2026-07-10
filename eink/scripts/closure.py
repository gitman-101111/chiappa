#!/usr/bin/env python3
"""Resolve the full DT_NEEDED closure for the e-ink runtime bundle against the
vendor rootfs. Prints resolved lib paths + any unresolved sonames."""
import os, subprocess, sys

V = os.environ.get("VROOT", "/mnt/vroot")
# search dirs for sonames (order = priority)
LIBDIRS = [f"{V}/usr/lib", f"{V}/lib", f"{V}/usr/lib/aarch64-linux-gnu",
           f"{V}/lib/aarch64-linux-gnu"]

# roots: the plugins we load + Qt libs our app links. Our app (qquick_m3)
# NEEDs Qt6Quick/Qml/Gui/Core/DBus + libstdc++; the epaper platform and
# qsgepaper backend pull the rest.
ROOTS = [
    f"{V}/usr/lib/plugins/platforms/libepaper.so",
    f"{V}/usr/lib/plugins/scenegraph/libqsgepaper.so",
    f"{V}/usr/lib/plugins/imageformats/libqgif.so",
    f"{V}/usr/lib/plugins/imageformats/libqico.so",
    f"{V}/usr/lib/plugins/imageformats/libqjpeg.so",
    f"{V}/usr/lib/plugins/imageformats/libqsvg.so",
    f"{V}/usr/lib/libQt6Quick.so.6",
    f"{V}/usr/lib/libQt6Qml.so.6",
    f"{V}/usr/lib/libQt6Gui.so.6",
    f"{V}/usr/lib/libQt6Core.so.6",
    f"{V}/usr/lib/libQt6DBus.so.6",
    f"{V}/usr/lib/libQt6QuickControls2.so.6",  # common QML UI dep
    f"{V}/usr/lib/libQt6Widgets.so.6",         # some styles need it
]

def needed(path):
    try:
        out = subprocess.check_output(["readelf", "-d", path],
                                      stderr=subprocess.DEVNULL, text=True)
    except subprocess.CalledProcessError:
        return []
    res = []
    for line in out.splitlines():
        if "(NEEDED)" in line and "[" in line:
            res.append(line[line.index("[")+1:line.index("]")])
    return res

def find(soname):
    for d in LIBDIRS:
        p = os.path.join(d, soname)
        if os.path.exists(p):
            return os.path.realpath(p)
    return None

resolved = {}   # soname -> realpath
unresolved = set()
stack = list(ROOTS)
# seed: treat roots as already-resolved files, enqueue their NEEDED
seen_files = set()
queue = []
for r in ROOTS:
    if os.path.exists(r):
        resolved[os.path.basename(r)] = os.path.realpath(r)
        queue.extend(needed(r))
    else:
        print(f"!! missing root: {r}", file=sys.stderr)

while queue:
    so = queue.pop()
    if so in resolved or so in unresolved:
        continue
    p = find(so)
    if p is None:
        unresolved.add(so)
        continue
    resolved[so] = p
    queue.extend(needed(p))

# libc/libm/etc resolve as sonames too; report them
print("# RESOLVED CLOSURE (%d libs)" % len(resolved))
total = 0
for so in sorted(resolved):
    p = resolved[so]
    sz = os.path.getsize(p)
    total += sz
    print(f"{p}\t{sz}")
print(f"# TOTAL {total} bytes = {total/1e6:.1f} MB", file=sys.stderr)
if unresolved:
    print("\n# UNRESOLVED (expected: libc/loader provided separately, or truly missing):",
          file=sys.stderr)
    for so in sorted(unresolved):
        print(f"#   {so}", file=sys.stderr)
