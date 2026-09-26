#!/bin/sh
# Refuse a U-Boot that the board's BL2 cannot load, or that cannot start.
#
#   check-bl33.sh <name> <u-boot.bin> <u-boot.lzma> <bin max> <lzma max>
#
# <lzma max> is BL2's: it reads the LZMA BL33 out of the FIP into a fixed
# buffer (EN7523_IMAGE_BUF_SIZE, plat/ecnt/en7523/include/platform_def.h).
# A larger image fails load_image() with "BL2: Failed to load image id 5
# (-27)".  The decoder then takes its probabilities from the rest of that
# buffer, 2 * (1846 + (768 << (lc + lp))) bytes plus up to 7 for alignment,
# or stops with "Failed to decompress image (err=2)".  The limits leave
# 16 KiB for that, which holds for lc + lp <= 3 (lzma and xz use 3 + 0).
#
# <bin max> is U-Boot's, not BL2's.  BL2 unpacks at BL33_BASE whatever size
# the LZMA header names, up to 5.5 MiB; its decoder ignores BL33_LIMIT.
# U-Boot then runs in place until it relocates, with its stack, global data
# and early malloc pool just below TEXT_BASE + 2 MiB (CFG_SYS_INIT_RAM_SIZE),
# so an image that reaches them overwrites itself.
#
# Either way the board does not boot until another FIP is sent over XMODEM,
# so the build stops here instead.
set -eu

name=$1 bin=$2 lzma=$3 bin_max=$(($4)) lzma_max=$(($5))
b=$(wc -c < "$bin")
l=$(wc -c < "$lzma")
b=$((b)) l=$((l))
p=$(od -An -tu1 -N1 "$lzma")
p=$((p))

echo "$name: U-Boot $b bytes (runs in place up to $bin_max)," \
     "LZMA $l bytes (BL2 loads up to $lzma_max)"
if [ $((p % 9 + p / 9 % 5)) -gt 3 ]; then
	echo "$name: $lzma has lc + lp > 3; BL2's decoder needs more" \
	     "than the 16 KiB left for it" >&2
	exit 1
fi
if [ "$b" -gt "$bin_max" ] || [ "$l" -gt "$lzma_max" ]; then
	echo "$name: U-Boot is too large to load and start; trim its defconfig" >&2
	exit 1
fi
