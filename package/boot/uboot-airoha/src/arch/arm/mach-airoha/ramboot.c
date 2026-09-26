// SPDX-License-Identifier: GPL-2.0
/*
 * Telling a U-Boot loaded over the serial port from one booted off flash.
 *
 * Loaded into RAM to look at a board, back it up or recover it, U-Boot has
 * no business booting the system on flash by itself: that is what the one
 * on flash is for, and it may well be why the board is on the bench.  The
 * "ramboot" command lets the boot scripts ask.
 */

#include <command.h>
#include <cpu_func.h>
#include <event.h>
#include <stdio.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <asm/unaligned.h>
#include <linux/string.h>
#include <linux/types.h>

/*
 * BL2 in the Airoha TF-A takes the BL31 + U-Boot FIP over XMODEM into its
 * memmap window (PLAT_ECNT_FIP_BASE, which CONFIG_SYS_LOAD_ADDR matches) and
 * boots it from there.  Booting off flash it reads the FIP out of the UBI
 * "fip" volume straight into place, and the window holds either nothing or,
 * on SPI NAND, a raw pre-load of the boot area -- whose FIP is the
 * preloader's own, with no BL33 in it.  So a FIP in the window carrying a
 * BL33 is one that came over the serial port.
 */
#define XMODEM_FIP		0x81800000UL
#define XMODEM_FIP_MAX		0x7f800
#define FIP_TOC_MAGIC		0xaa640001
#define FIP_TOC_HDR		16
#define FIP_TOC_ENTRY		40
#define FIP_TOC_MAX_ENTRIES	16

static const u8 uuid_bl33[16] = {
	0xd6, 0xd0, 0xee, 0xa7, 0xfc, 0xea, 0xd5, 0x4b,
	0x97, 0x82, 0x99, 0x34, 0xf2, 0x34, 0xb6, 0xe4,
};

static bool ramboot;

bool airoha_ramboot(void)
{
	return ramboot;
}

static bool fip_has_bl33(const u8 *toc)
{
	static const u8 none[16];
	const u8 *e;
	u64 off, size;
	int i;

	for (i = 0; i < FIP_TOC_MAX_ENTRIES; i++) {
		e = toc + FIP_TOC_HDR + i * FIP_TOC_ENTRY;
		if (!memcmp(e, none, sizeof(none)))
			return false;
		if (memcmp(e, uuid_bl33, sizeof(uuid_bl33)))
			continue;
		off = get_unaligned_le64(e + 16);
		size = get_unaligned_le64(e + 24);

		return size && off < XMODEM_FIP_MAX &&
		       size <= XMODEM_FIP_MAX - off;
	}

	return false;
}

/*
 * Once, early -- after the environment, before anything can be loaded to
 * the window -- and the header wiped behind it: DRAM keeps its contents
 * across a warm reset, and the next boot, off flash, must not find this
 * FIP again.  Wiped to DRAM, not just to the cache, which a reset drops.
 */
static int detect_ramboot(void)
{
	u8 *toc = (u8 *)XMODEM_FIP;

	if (get_unaligned_le32(toc) != FIP_TOC_MAGIC)
		return 0;

	ramboot = fip_has_bl33(toc);
	writel(0, toc);
	flush_dcache_range(XMODEM_FIP, XMODEM_FIP + ARCH_DMA_MINALIGN);

	if (ramboot)
		printf("Loaded over the serial port: the system on flash is not booted by itself\n");

	return 0;
}
EVENT_SPY_SIMPLE(EVT_SETTINGS_R, detect_ramboot);

static int do_ramboot(struct cmd_tbl *cmdtp, int flag, int argc,
		      char *const argv[])
{
	return ramboot ? CMD_RET_SUCCESS : CMD_RET_FAILURE;
}

U_BOOT_CMD(ramboot, 1, 0, do_ramboot,
	   "succeed if this U-Boot was loaded over the serial port",
	   "");
