#!/usr/bin/env python3
"""Build the chainloader slot of a locked Gemtek AN7581 board.

Usage: mkslot.py --uboot u-boot.lzma --dtb u-boot.dtb --shim shim.bin
                 --mkimage tools/mkimage --dtc scripts/dtc/dtc [--xz xz]
                 --name <description> --out slot.bin [--fit out.itb]

What gets written to the chainloader partition (flash 0x600000, 1 MiB):

  0x0000  legacy uImage: the prefix shim (shim.c), LZMA-compressed, for
          "flash read 0x600000 ...; bootm"
  0x2100  FIT: U-Boot as an LZMA "kernel" at 0x80200000 plus its DTB, for
          the stock "flash read 0x602100 ...; bootm"

The FIT is the one the W1700K chainloader recipe has booted from its vendor
U-Boot all along (scripts/mkits.sh -k u-boot.lzma -C lzma -a/-e 0x80200000
-c conf-uboot -s 0x82000000), so the stock path does not depend on the shim.
The shim is told where the LZMA stream sits inside the FIT by rewriting its
parameter block, found by magic.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import time
import zlib

FIT_OFF = 0x2100
SLOT_SIZE = 0x100000
LOAD = 0x80200000
FDT_LOAD = 0x82000000

MAGIC = b"XRSLOT01"
# magic, link, base, off, len, crc, dest, room, uart, pad
PARAM = struct.Struct("<8sQ8I")

IH_MAGIC = 0x27051956
IH_OS_LINUX, IH_ARCH_ARM64, IH_TYPE_KERNEL, IH_COMP_LZMA = 5, 22, 2, 3
UIMAGE = struct.Struct(">7I4B32s")

ITS = """/dts-v1/;

/ {{
	description = "ARM64 OpenWrt FIT (Flattened Image Tree)";
	#address-cells = <1>;

	images {{
		kernel-1 {{
			description = "{name}";
			data = /incbin/("{uboot}");
			type = "kernel";
			arch = "arm64";
			os = "linux";
			compression = "lzma";
			load = <{load:#x}>;
			entry = <{load:#x}>;
			hash-1 {{
				algo = "crc32";
			}};
			hash-2 {{
				algo = "sha1";
			}};
		}};

		fdt-1 {{
			description = "{name} device tree blob";
			data = /incbin/("{dtb}");
			type = "flat_dt";
			load = <{fdt_load:#x}>;
			arch = "arm64";
			compression = "none";
			hash-1 {{
				algo = "crc32";
			}};
			hash-2 {{
				algo = "sha1";
			}};
		}};
	}};

	configurations {{
		default = "conf-uboot";
		conf-uboot {{
			description = "{name}";
			kernel = "kernel-1";
			fdt = "fdt-1";
		}};
	}};
}};
"""


def die(msg):
    sys.exit("mkslot: " + msg)


def build_fit(args, lz_path):
    with tempfile.TemporaryDirectory() as tmp:
        its = os.path.join(tmp, "slot.its")
        itb = os.path.join(tmp, "slot.itb")
        with open(its, "w") as f:
            f.write(ITS.format(name=args.name, uboot=os.path.abspath(lz_path),
                               dtb=os.path.abspath(args.dtb), load=LOAD,
                               fdt_load=FDT_LOAD))
        # mkimage runs "dtc" off PATH; use the one that built U-Boot.
        env = dict(os.environ)
        env["PATH"] = os.path.dirname(os.path.abspath(args.dtc)) + \
            os.pathsep + env.get("PATH", "")
        cmd = [args.mkimage, "-f", its, itb]
        epoch = os.environ.get("SOURCE_DATE_EPOCH")
        if epoch:
            env["SOURCE_DATE_EPOCH"] = epoch
        r = subprocess.run(cmd, env=env, capture_output=True, text=True)
        if r.returncode:
            sys.stderr.write(r.stdout + r.stderr)
            die("mkimage failed")
        return open(itb, "rb").read()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    for opt in ("uboot", "dtb", "shim", "mkimage", "dtc", "name", "out"):
        ap.add_argument("--" + opt, required=True)
    ap.add_argument("--xz", default="xz", help="xz, for the .lzma of the shim")
    ap.add_argument("--fit", help="also write the bare FIT here")
    args = ap.parse_args()

    lz = open(args.uboot, "rb").read()
    if len(lz) <= 13:
        die("%s is not an .lzma file" % args.uboot)
    size = struct.unpack_from("<Q", lz, 5)[0]

    fit = build_fit(args, args.uboot)
    if fit[:4] != b"\xd0\x0d\xfe\xed":
        die("mkimage did not produce a FIT")
    at = fit.find(lz)
    if at < 0 or fit.find(lz, at + 1) >= 0:
        die("cannot place the LZMA stream inside the FIT")

    shim = bytearray(open(args.shim, "rb").read())
    p = shim.find(MAGIC)
    if p < 0 or shim.find(MAGIC, p + 1) >= 0:
        die("no single %s parameter block in %s" % (MAGIC.decode(), args.shim))
    (_, link, base, _, _, _, dest, room, uart,
     pad) = PARAM.unpack_from(shim, p)
    if dest != LOAD:
        die("the shim unpacks to 0x%x, the FIT loads at 0x%x" % (dest, LOAD))
    if size != 2 ** 64 - 1 and size > room:
        die("U-Boot unpacks to %d bytes, the shim has room for %d"
            % (size, room))
    PARAM.pack_into(shim, p, MAGIC, link, base, FIT_OFF + at, len(lz),
                    zlib.crc32(lz), dest, room, uart, pad)

    # Compressed, or the shim alone would not fit ahead of the FIT: the
    # LZMA decoder in it is most of its 10 KiB.  The vendor bootm unpacks
    # a legacy lzma kernel itself; the other chainloader for this board
    # ships its prefix shim the same way (.lzma, size not recorded).
    r = subprocess.run([args.xz, "--format=lzma", "-9", "-c"],
                       input=bytes(shim), capture_output=True)
    if r.returncode:
        sys.stderr.write(r.stderr.decode(errors="replace"))
        die("xz failed")
    data = r.stdout

    stamp = int(os.environ.get("SOURCE_DATE_EPOCH") or time.time())
    name = b"Web U-Boot chainloader shim"
    fields = [IH_MAGIC, 0, stamp, len(data), link, link, zlib.crc32(data),
              IH_OS_LINUX, IH_ARCH_ARM64, IH_TYPE_KERNEL, IH_COMP_LZMA, name]
    fields[1] = zlib.crc32(UIMAGE.pack(*fields))
    hdr = UIMAGE.pack(*fields)

    prefix = hdr + data
    if len(prefix) > FIT_OFF:
        die("the prefix is %d bytes, more than the 0x%x before the FIT"
            % (len(prefix), FIT_OFF))
    slot = prefix + b"\0" * (FIT_OFF - len(prefix)) + fit
    if len(slot) > SLOT_SIZE:
        die("the slot is %d bytes, the chainloader partition holds %d"
            % (len(slot), SLOT_SIZE))

    open(args.out, "wb").write(slot)
    if args.fit:
        open(args.fit, "wb").write(fit)
    print("mkslot: %s, %d bytes: shim %d (%d packed) for 0x%x, FIT %d at "
          "0x%x, U-Boot LZMA %d at 0x%x"
          % (args.out, len(slot), len(shim), len(data), link, len(fit),
             FIT_OFF, len(lz), FIT_OFF + at))


if __name__ == "__main__":
    main()
