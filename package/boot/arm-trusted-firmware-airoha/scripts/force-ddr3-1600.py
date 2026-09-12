#!/usr/bin/env python3
"""Force AN7581 PCDDR3 to 1600 MT/s in the closed BL22 DRAMC blob.

dramc_pi_main.o's gFreqTbl_PC3 starts with freq_sel=11 (933 MHz CK = DDR3-1866).
Sel 12 is 800 MHz CK = DDR3-1600. Training can pass at 1866 and still corrupt
BL23 while LZMA-decompressing it into DRAM (LZMA res=1). DDR4 boards use
gFreqTbl_PC4 and are not touched.
"""
import pathlib
import sys

# first two fields of gFreqTbl_PC3: freq_sel=11, second=2
NEEDLE = bytes.fromhex("0b00000002000000")
PATCH = bytes.fromhex("0c00000002000000")

TARGETS = (
    "plat/ecnt/an7581/bl22/dramc_pi_main.o",
    "plat/ecnt/an7581/bl31/dramc_pi_main.o",
)


def patch(path: pathlib.Path) -> None:
    data = path.read_bytes()
    n = data.count(NEEDLE)
    if n != 1:
        raise SystemExit(f"{path}: expected 1 match of gFreqTbl_PC3 head, found {n}")
    path.write_bytes(data.replace(NEEDLE, PATCH, 1))
    print(f"patched {path}: DDR3 freq_sel 11 (1866) -> 12 (1600)")


def main() -> None:
    root = pathlib.Path(sys.argv[1])
    found = False
    for rel in TARGETS:
        p = root / rel
        if p.is_file():
            patch(p)
            found = True
    if not found:
        raise SystemExit(f"{root}: no dramc_pi_main.o to patch")


if __name__ == "__main__":
    main()
