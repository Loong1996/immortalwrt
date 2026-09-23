#!/usr/bin/env python3
"""Assemble web-recovery defconfigs and default environments.

Run this after src/ has been copied onto the U-Boot tree and the patches
have been applied. OpenWrt's Build/Prepare does that, and so does the
uboot-check workflow. The files under this directory are the only copy.

defconfig layers, included with #include so a later board can stop at
the layer it actually wants:

  airoha_web_defconfig
      The recovery page: httpd, TCP, cyclic, the event spies, console
      record. A chainloader board includes this and nothing below it.
  an7581_web_defconfig / an7583_web_defconfig
      Replace-bootloader bundles. SoC options, EN8811, whole-flash
      stock restore, MMC off. A locked bootloader does not include these.

env layers, concatenated because the text-file environment reader drops
lines that start with #:

  menu.env
      Title, web_uboot_envver, about, the recovery menu entries, reset
      button into httpd. Safe to share with a chainloader.
  replace-bootloader.env
      BL2, FIP and the UBI boot scripts. Not for a locked bootloader.
  policy-factory.env / policy-foreign.env
      Factory MAC and volumes, or the "UBI with no fip is stock" guard.

A board file picks a bundle, an env fragment list and a policy. The
default fragment list is menu.env plus replace-bootloader.env. Boot
file names come from the image profile: immortalwrt-airoha-<soc>-<profile>-*.
"""

import argparse
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent

# Same order the four boards already ship, so a diff of the assembled
# file stays readable. A policy key that is not in this list is appended.
ORDER = [
    "ipaddr", "serverip", "loadaddr", "console", "bootcmd", "bootconf",
    "bootdelay", "bootfile", "bootfile_bl2", "bootfile_fip", "bootfile_upg",
    "bootled_status", "web_uboot_envver", "bootmenu_confirm_return",
    "bootmenu_default", "bootmenu_delay", "bootmenu_title", "bootmenu_0",
    "bootmenu_0d", "bootmenu_1", "bootmenu_2", "bootmenu_3", "bootmenu_4",
    "bootmenu_5", "bootmenu_6", "bootmenu_7", "bootmenu_8", "bootmenu_9",
    "boot_first", "boot_default", "boot_production", "boot_ubi",
    "boot_tftp_forever", "web_uboot_boot_forever", "boot_tftp_production",
    "boot_tftp_recovery", "boot_tftp", "boot_tftp_write_bl2",
    "boot_tftp_write_fip", "preboot", "check_buttons", "web_uboot_show_about",
    "ethaddr_factory", "part_default", "reset_factory", "mtd_write_bl2",
    "web_uboot_write_bl2", "web_uboot_write_fip", "ubi_create_board_data",
    "ubi_create_env", "ubi_format", "web_uboot_format_ubi",
    "ubi_prepare_rootfs", "ubi_read_production", "ubi_remove_rootfs",
    "ubi_write_fip", "ubi_write_production", "_init_env", "_firstboot",
    "web_uboot_no_ubi", "web_uboot_foreign_ubi", "_switch_to_menu",
    "_bootmenu_update_title",
]

VERSION_RE = re.compile(r'^\s*#define\s+WEB_VERSION\s+"([^"]+)"', re.M)
SUFFIXES = (
    ("bootfile", "-initramfs-recovery.itb"),
    ("bootfile_bl2", "-preloader.bin"),
    ("bootfile_fip", "-bl31-uboot.fip"),
    ("bootfile_upg", "-squashfs-sysupgrade.itb"),
)


def die(msg):
    print(f"web-uboot: {msg}", file=sys.stderr)
    sys.exit(1)


def read_board(path):
    board = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            die(f"{path.name}: 无法解析 {raw!r}")
        key, value = line.split("=", 1)
        board[key.strip()] = value.strip()
    for key in ("soc", "profile", "dts", "fdt", "led", "bundle"):
        if not board.get(key):
            die(f"{path.name}: 缺少 {key}")
    vols, mac = board.get("factory_vols"), board.get("factory_mac")
    if bool(vols) != bool(mac):
        die(f"{path.name}: factory_vols 和 factory_mac 要一起写")
    return board


def parse_env(text, version):
    pairs = []
    for raw in text.splitlines():
        if not raw or raw.startswith("#"):
            continue
        line = raw.replace("@WEB_VERSION@", version)
        if "@" in line:
            die(f"环境模板里还有未替换的标记: {line}")
        key, sep, value = line.partition("=")
        if not sep:
            die(f"环境模板缺少 '=': {line}")
        pairs.append((key, value))
    return pairs


def find_version(tree, explicit):
    if explicit:
        return explicit
    # Only net/httpd.c defines it. Reading just that file rather than
    # every .c/.h in the tree keeps Build/Prepare fast, and an unrelated
    # upstream WEB_VERSION can never be picked up.
    path = tree / "net" / "httpd.c"
    if not path.is_file():
        die(f"没有找到 {path}")
    found = VERSION_RE.findall(path.read_text(encoding="utf-8",
                                              errors="replace"))
    if not found:
        die(f"{path} 里没有 #define WEB_VERSION")
    if len(set(found)) > 1:
        die("WEB_VERSION 有多个不同的值: " + ", ".join(sorted(set(found))))
    return found[0]


def assemble_env(board, version):
    fragments = board.get("fragments", "menu.env replace-bootloader.env").split()
    policy = board.get("policy")
    if policy:
        fragments.append(f"policy-{policy}.env")
    merged = {}
    for name in fragments:
        path = HERE / "env" / name
        if not path.is_file():
            die(f"没有环境片段 {name}")
        for key, value in parse_env(path.read_text(encoding="utf-8"), version):
            if key in merged:
                die(f"环境变量 {key} 在多个片段里出现")
            merged[key] = value
    prefix = f"immortalwrt-airoha-{board['soc']}-{board['profile']}"
    for key, suffix in SUFFIXES:
        if key in merged:
            die(f"环境变量 {key} 应由配方名生成，不要写进片段")
        merged[key] = prefix + suffix
    if "bootled_status" in merged:
        die("bootled_status 写在板子文件的 led= 里")
    merged["bootled_status"] = board["led"]
    unknown = [key for key in merged if key not in ORDER]
    if unknown:
        die("环境变量不在输出顺序里，先加进 assemble.py 的 ORDER: " + ", ".join(unknown))
    lines = [f"{key}={merged[key]}" for key in ORDER if key in merged]
    return "\n".join(lines) + "\n"


def assemble_defconfig(name, board):
    bundle = HERE / "defconfig" / board["bundle"]
    if not bundle.is_file():
        die(f"{name}: 没有 defconfig 包 {board['bundle']}")
    lines = [
        f"#include <configs/{board['bundle']}>",
        f'CONFIG_DEFAULT_DEVICE_TREE="{board["dts"]}"',
        f'CONFIG_DEFAULT_FDT_FILE="{board["fdt"]}"',
        f'CONFIG_ENV_DEFAULT_ENV_TEXT_FILE="defenvs/{name}_env"',
        f'CONFIG_DEFAULT_ENV_FILE="defenvs/{name}_env"',
    ]
    if board.get("factory_vols"):
        lines.append(f'CONFIG_HTTPD_FACTORY_VOLS="{board["factory_vols"]}"')
        lines.append(f'CONFIG_HTTPD_FACTORY_MAC="{board["factory_mac"]}"')
    extra = board.get("extra_defconfig")
    if extra:
        path = HERE / extra
        if not path.is_file():
            die(f"{name}: 没有 {extra}")
        lines.extend(
            line for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.startswith("#")
        )
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dest", required=True, type=pathlib.Path,
                        help="U-Boot tree to write configs/ and defenvs/ into")
    parser.add_argument("--tree", type=pathlib.Path,
                        help="patched U-Boot tree to read WEB_VERSION from")
    parser.add_argument("--version", help="override WEB_VERSION, for checks")
    args = parser.parse_args()
    if args.tree:
        version = find_version(args.tree, args.version)
    elif args.version:
        version = args.version
    else:
        die("需要 --tree 或 --version")

    configs = args.dest / "configs"
    envs = args.dest / "defenvs"
    configs.mkdir(parents=True, exist_ok=True)
    envs.mkdir(parents=True, exist_ok=True)
    for src in (HERE / "defconfig").glob("*"):
        if src.is_file():
            (configs / src.name).write_text(
                src.read_text(encoding="utf-8"), encoding="utf-8", newline="\n")

    boards = sorted((HERE / "boards").glob("*"))
    if not boards:
        die("boards/ 是空的")
    for path in boards:
        if not path.is_file():
            continue
        board = read_board(path)
        name = path.name
        (configs / f"{name}_defconfig").write_text(
            assemble_defconfig(name, board), encoding="utf-8", newline="\n")
        (envs / f"{name}_env").write_text(
            assemble_env(board, version), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
