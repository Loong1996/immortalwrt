#!/bin/sh
# Build the chainloader slot out of a compiled U-Boot tree.
#
#   build.sh <cross-prefix> <u-boot tree> <u-boot.lzma> <name> <slot out>
#
# The shim is freestanding: LzmaDec.c from the U-Boot tree, the stub headers
# in inc/ in place of U-Boot's, no libc.  mkslot.py then puts the FIT
# together with the tree's own mkimage and dtc, and packs the shim with
# $XZ (default: xz off PATH).
set -eu

cross=$1 tree=$2 lzma=$3 name=$4 out=$5
here=$(cd "$(dirname "$0")" && pwd)
work=$tree/chainload
mkdir -p "$work"

cflags="-Os -Wall -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns
	-mgeneral-regs-only -mstrict-align -fno-pic -fno-pie -fno-stack-protector
	-fno-asynchronous-unwind-tables -fno-unwind-tables -ffunction-sections
	-fdata-sections -I $here/inc -I $tree/lib/lzma"

# -Werror for what is ours; LzmaDec.c is U-Boot's, and a newer compiler's
# new warning in it is no reason to stop the build.
"${cross}gcc" $cflags -Werror -c -o "$work/start.o" "$here/start.S"
"${cross}gcc" $cflags -Werror -c -o "$work/shim.o" "$here/shim.c"
"${cross}gcc" $cflags -c -o "$work/lzmadec.o" "$tree/lib/lzma/LzmaDec.c"
"${cross}gcc" -nostdlib -static -no-pie -Wl,--build-id=none -Wl,--gc-sections \
	-Wl,-T,"$here/shim.lds" -o "$work/shim.elf" \
	"$work/start.o" "$work/shim.o" "$work/lzmadec.o" -lgcc
"${cross}objcopy" -O binary "$work/shim.elf" "$work/shim.bin"

python3 "$here/mkslot.py" --uboot "$lzma" --dtb "$tree/u-boot.dtb" \
	--shim "$work/shim.bin" --mkimage "$tree/tools/mkimage" \
	--dtc "$tree/scripts/dtc/dtc" --xz "${XZ:-xz}" --name "$name" --out "$out"
