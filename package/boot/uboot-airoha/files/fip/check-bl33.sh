#!/bin/sh
# Refuse a U-Boot that the board's BL2 cannot load.
#
#   check-bl33.sh <name> <u-boot.bin> <u-boot.lzma> <bin max> <lzma max>
#
# BL2 reads the LZMA BL33 out of the FIP into a fixed buffer, which also has
# to hold the ~16 KiB the decoder allocates, and unpacks it into a fixed
# window at BL33_BASE (plat/ecnt/en7523/include/platform_def.h).  Past
# either, it stops with "BL2: Failed to load image id 5 (-27)" and the board
# does not boot until another FIP is sent over XMODEM.  So the build stops
# here instead.
set -eu

name=$1 bin=$2 lzma=$3 bin_max=$(($4)) lzma_max=$(($5))
b=$(wc -c < "$bin")
l=$(wc -c < "$lzma")
b=$((b)) l=$((l))

echo "$name: U-Boot $b bytes (BL2 unpacks up to $bin_max)," \
     "LZMA $l bytes (up to $lzma_max)"
if [ "$b" -gt "$bin_max" ] || [ "$l" -gt "$lzma_max" ]; then
	echo "$name: U-Boot is too large for its BL2; trim its defconfig" >&2
	exit 1
fi
