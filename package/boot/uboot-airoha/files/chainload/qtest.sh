#!/bin/sh
# Run the prefix shim under QEMU, the way the vendor bootm would, and check
# that it unpacks what mkslot.py pointed it at and jumps there.
#
#   qtest.sh <u-boot tree>      (after build.sh; needs qemu-system-aarch64,
#                                socat, xz, python3)
#
# The payload is not U-Boot, which would only crash on a machine that is not
# an AN7581, but 900 KB whose first word is "b ." -- so a shim that got there
# sits still at 0x80200000 with the payload intact behind it.  Both .lzma
# header styles (size recorded, as OpenWrt's lzma writes it; size unknown, as
# xz writes it), at EL1, EL2 and EL3.  The UART is switched off in the
# parameter block: QEMU's virt board has none at the AN7581's address.
set -eu

tree=$(cd "$1" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cd "$T"

python3 - <<'PY'
import random, struct
random.seed(1)
body = bytes(random.getrandbits(8) for _ in range(300000)) + bytes(600000)
open("payload.bin", "wb").write(struct.pack("<I", 0x14000000) + body)
PY
xz --format=lzma -9 -c payload.bin > p-unknown.lzma
python3 - <<'PY'
import lzma, struct
d = open("payload.bin", "rb").read()
raw = lzma.compress(d, format=lzma.FORMAT_RAW, filters=[{"id": lzma.FILTER_LZMA1,
      "dict_size": 1 << 23, "lc": 3, "lp": 0, "pb": 2}])
hdr = bytes([(2 * 5 + 0) * 9 + 3]) + struct.pack("<I", 1 << 23)
open("p-known.lzma", "wb").write(hdr + struct.pack("<Q", len(d)) + raw)
PY

mon() { (echo "$1"; sleep 1) | socat - "UNIX-CONNECT:$T/mon" | tr -d '\r'; }

rc=0
for kind in known unknown; do
	python3 "$here/mkslot.py" --uboot p-$kind.lzma --dtb "$tree/u-boot.dtb" \
		--shim "$tree/chainload/shim.bin" --mkimage "$tree/tools/mkimage" \
		--dtc "$tree/scripts/dtc/dtc" --name qtest --out slot.bin >/dev/null
	# The shim as the vendor bootm unpacks it, with the UART off.
	python3 - slot.bin shim.bin <<'PY'
import lzma, struct, sys
s = open(sys.argv[1], "rb").read()
size = struct.unpack(">I", s[12:16])[0]
shim = bytearray(lzma.decompress(s[64:64 + size], format=lzma.FORMAT_ALONE))
p = shim.find(b"XRSLOT01")
struct.pack_into("<I", shim, p + 8 + 8 + 4 * 6, 0)
open(sys.argv[2], "wb").write(shim)
PY
	for m in virt virt,virtualization=on virt,secure=on,virtualization=on; do
		rm -f mon mem.bin
		qemu-system-aarch64 -M $m -cpu cortex-a53 -m 3G -nographic \
			-serial none -nic none -monitor unix:$T/mon,server,nowait \
			-device loader,file=shim.bin,addr=0x81000000 \
			-device loader,file=slot.bin,addr=0x81800000 \
			-device loader,addr=0x81000000,cpu-num=0 >/dev/null 2>&1 &
		q=$!
		pc= el= i=0
		while [ $i -lt 120 ]; do
			sleep 1
			i=$((i + 1))
			[ -S mon ] || continue
			r=$(mon "info registers")
			pc=$(echo "$r" | grep -o 'PC=[0-9a-f]*' | head -1)
			el=$(echo "$r" | grep -o 'EL[0-3][htn]*' | head -1)
			[ "$pc" = PC=0000000080200000 ] && break
		done
		mon "pmemsave 0x80200000 $(wc -c < payload.bin) \"$T/mem.bin\"" >/dev/null
		sleep 1
		mon quit >/dev/null || true
		wait $q || true
		if [ "$pc" = PC=0000000080200000 ] && cmp -s mem.bin payload.bin; then
			echo "qtest: $kind size, $m ($el): unpacked and jumped"
		else
			echo "qtest: $kind size, $m: FAILED, $pc $el after ${i}s"
			rc=1
		fi
	done
done
exit $rc
