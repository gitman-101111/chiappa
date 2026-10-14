#!/usr/bin/env bash
# Regenerate config-chiappa-mainline.aarch64 (the COMPLETE config the kernel
# is built from) out of the arm64 defconfig of a mainline tree plus the
# chiappa-mainline.config fragment. Run it whenever the fragment or the kernel
# series changes; the full config is what gets committed, so a build never
# depends on olddefconfig picking new defaults at build time.
#
# Usage: regenerate-config.sh <patched-mainline-source-tree>
# Needs an aarch64 cross toolchain on PATH (aarch64-unknown-linux-gnu-gcc) and
# the usual kconfig host tools (flex, bison, gcc).
set -euo pipefail

src=${1:?mainline source tree}
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

export ARCH=arm64 CROSS_COMPILE=${CROSS_COMPILE:-aarch64-unknown-linux-gnu-}
make -C "$src" -s O="$out" defconfig
"$src/scripts/kconfig/merge_config.sh" -m -O "$out" "$out/.config" "$here/chiappa-mainline.config" >/dev/null
make -C "$src" -s O="$out" olddefconfig

# Options the fragment asked for that the merged result dropped are a mistake
# somewhere (misspelt symbol, unmet dependency); refuse to publish them. A
# symbol this kernel does not define at all is only reported: the fragment
# may name options that exist on one supported kernel series and not another.
status=0
defined() { grep -rqE "^(menu)?config ${1#CONFIG_}$" --include="Kconfig*" "$src"; }
while IFS= read -r line; do
  case $line in
    CONFIG_*=*) grep -qxF "$line" "$out/.config" || { if defined "${line%%=*}"; then echo "lost: $line" >&2; status=1; else echo "not in this kernel: $line" >&2; fi; } ;;
    "# CONFIG_"*" is not set") sym=${line#\# }; sym=${sym% is not set}; grep -qE "^$sym=" "$out/.config" && { echo "still set: $sym" >&2; status=1; } ;;
  esac
done < "$here/chiappa-mainline.config"

cp "$out/.config" "$here/config-chiappa-mainline.aarch64"
echo "wrote $here/config-chiappa-mainline.aarch64"
exit $status
