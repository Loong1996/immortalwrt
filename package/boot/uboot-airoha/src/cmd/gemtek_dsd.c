// SPDX-License-Identifier: GPL-2.0+
/*
 * gemtek_dsd -- factory data of a Gemtek AN7581 board, taken from where the
 * vendor keeps it.
 *
 * The XR1710G (and the W1700K it is built on) ships with a locked vendor
 * bootloader, so its flash keeps the vendor layout up front: a "vendor"
 * partition over the DSD area at flash offset 0x400000, which holds a text
 * block of name=value lines (lan_mac=, wan_mac=, ...) and, at +0x5000, the
 * Wi-Fi calibration EEPROM.  The partition starts at 0 when the vendor
 * bootloader chainloads this one and at 0x20000 when this one's BL2 has
 * taken the first erase block, so the DSD is addressed on the flash.
 * OpenWrt on these boards does not read the DSD.  It reads a UBI volume
 * named "factory" laid out as
 *
 *	0x0000	EEPROM, 0x1e00 bytes
 *	0x5000	WAN MAC
 *	0x6000	LAN MAC (mac-base: Wi-Fi and the other ports count up from it)
 *
 * and that volume goes with everything else when UBI is rebuilt.  The DSD
 * does not: nothing on this side ever writes the vendor partition.  So the
 * volume is regenerated from the DSD, and kept in step with it on every
 * boot -- the vendor partition is also where a factory-data editor in the
 * firmware writes, and this is what carries such an edit into the volume.
 *
 * Both halves are checked before anything is written: an EEPROM that does
 * not start with the MT7990 chip ID, or a MAC that is not a valid unicast
 * address, leaves the volume alone.  A block the vendor bootloader has
 * remapped through its BMT reads here as whatever the bad block holds, and
 * that must not end up as a board's calibration data.
 *
 *	gemtek_dsd sync		create the factory volume, or rewrite it if it
 *				differs from the DSD (UBI must be attached)
 *	gemtek_dsd ethaddr	set ethaddr (LAN) and eth1addr (WAN)
 */

#include <command.h>
#include <env.h>
#include <malloc.h>
#include <mtd.h>
#include <net.h>
#include <ubi_uboot.h>
#include <linux/err.h>
#include <linux/string.h>
#include <vsprintf.h>

#define DSD_PART	"vendor"
#define DSD_OFF		0x400000	/* on the flash, not in DSD_PART */
#define DSD_TEXT_LEN	0x1000
#define DSD_EEPROM_OFF	(DSD_OFF + 0x5000)

#define FACTORY_VOL	"factory"
#define EEPROM_LEN	0x1e00
#define FAC_WAN_MAC	0x5000
#define FAC_LAN_MAC	0x6000
#define FACTORY_LEN	(FAC_LAN_MAC + ARP_HLEN)

static int dsd_read(loff_t off, size_t len, void *buf)
{
	struct mtd_info *mtd;
	size_t rl = 0;
	int ret;

	mtd_probe_devices();
	mtd = get_mtd_device_nm(DSD_PART);
	if (IS_ERR_OR_NULL(mtd)) {
		printf("gemtek_dsd: no \"%s\" partition\n", DSD_PART);
		return -ENODEV;
	}

	if (off < mtd->offset || off - mtd->offset + len > mtd->size) {
		printf("gemtek_dsd: 0x%llx is outside \"%s\"\n",
		       (unsigned long long)off, DSD_PART);
		put_mtd_device(mtd);
		return -EINVAL;
	}
	ret = mtd_read(mtd, off - mtd->offset, len, &rl, buf);
	put_mtd_device(mtd);

	/* A corrected bit-flip is a read that worked. */
	if (ret == -EUCLEAN)
		ret = 0;
	if (!ret && rl != len)
		ret = -EIO;
	if (ret)
		printf("gemtek_dsd: reading %s at 0x%llx failed (%d)\n",
		       DSD_PART, (unsigned long long)off, ret);

	return ret;
}

/* The value of "key=" at the start of a line, as a MAC. */
static int dsd_mac(const char *text, int len, const char *key, u8 *mac)
{
	int klen = strlen(key);
	const char *p = text, *end = text + len;

	while (p < end) {
		const char *eol = memchr(p, '\n', end - p);
		int n;

		if (!eol)
			eol = end;
		n = eol - p;
		if (n > klen && !memcmp(p, key, klen)) {
			char v[ARP_HLEN_ASCII + 1];

			n -= klen;
			if (n > ARP_HLEN_ASCII)
				n = ARP_HLEN_ASCII;
			memcpy(v, p + klen, n);
			v[n] = '\0';
			string_to_enetaddr(v, mac);

			return is_valid_ethaddr(mac) ? 0 : -EINVAL;
		}
		p = eol + 1;
	}

	return -ENOENT;
}

static int dsd_macs(u8 *lan, u8 *wan)
{
	char *text;
	int ret;

	text = malloc(DSD_TEXT_LEN);
	if (!text)
		return -ENOMEM;

	ret = dsd_read(DSD_OFF, DSD_TEXT_LEN, text);
	if (!ret)
		ret = dsd_mac(text, DSD_TEXT_LEN, "lan_mac=", lan);
	if (!ret)
		ret = dsd_mac(text, DSD_TEXT_LEN, "wan_mac=", wan);
	if (ret)
		printf("gemtek_dsd: no valid lan_mac / wan_mac in the DSD (%d)\n",
		       ret);
	free(text);

	return ret;
}

/* What the factory volume should hold, 0xff where the DSD has nothing. */
static int dsd_factory(u8 *buf)
{
	int ret;

	memset(buf, 0xff, FACTORY_LEN);

	ret = dsd_read(DSD_EEPROM_OFF, EEPROM_LEN, buf);
	if (ret)
		return ret;
	/* MT7990 calibration data opens with its chip ID, little endian. */
	if (buf[0] != 0x90 || buf[1] != 0x79) {
		printf("gemtek_dsd: no Wi-Fi EEPROM in the DSD (starts %02x %02x)\n",
		       buf[0], buf[1]);
		return -EINVAL;
	}

	return dsd_macs(buf + FAC_LAN_MAC, buf + FAC_WAN_MAC);
}

/* The attached UBI's volume by name; cmd/ubi.c keeps its own lookup static. */
static struct ubi_volume *factory_vol(struct ubi_device *ubi)
{
	int i;

	for (i = 0; i < ubi->vtbl_slots; i++) {
		struct ubi_volume *v = ubi->volumes[i];

		if (v && !strcmp(v->name, FACTORY_VOL))
			return v;
	}

	return NULL;
}

static int do_sync(void)
{
	static char part[] = CONFIG_ENV_UBI_PART;
	static char name[] = FACTORY_VOL;
	struct ubi_device *ubi;
	struct ubi_volume *vol;
	u64 room = 0;
	long long used = 0;
	u8 *want, *have;
	char cmd[64];
	int ret;

	if (ubi_part(part, NULL))
		return CMD_RET_FAILURE;

	want = malloc(FACTORY_LEN);
	have = malloc(FACTORY_LEN);
	if (!want || !have) {
		ret = -ENOMEM;
		goto out;
	}

	ret = dsd_factory(want);
	if (ret)
		goto out;

	ubi = ubi_get_device(0);
	if (!ubi) {
		ret = -ENODEV;
		goto out;
	}
	vol = factory_vol(ubi);
	if (vol) {
		room = (u64)vol->reserved_pebs * vol->usable_leb_size;
		used = vol->used_bytes;
	}
	ubi_put_device(ubi);

	if (vol && used >= FACTORY_LEN &&
	    !ubi_volume_read(name, (char *)have, 0, FACTORY_LEN) &&
	    !memcmp(want, have, FACTORY_LEN)) {
		printf("gemtek_dsd: %s volume matches the DSD\n", FACTORY_VOL);
		goto out;
	}

	/* Too small to take it: start the volume over. */
	if (vol && room < FACTORY_LEN) {
		ret = run_command("ubi remove " FACTORY_VOL, 0);
		if (ret)
			goto out;
		vol = NULL;
	}
	/*
	 * Static, like every other copy of this volume on these boards: its
	 * size is then exactly FACTORY_LEN, which is what a backup of it
	 * downloads as and what the recovery page expects to be handed back.
	 */
	if (!vol) {
		snprintf(cmd, sizeof(cmd), "ubi create %s 0x%x static",
			 FACTORY_VOL, FACTORY_LEN);
		ret = run_command(cmd, 0);
		if (ret)
			goto out;
	}

	ret = ubi_volume_write(name, want, 0, FACTORY_LEN);
	if (!ret)
		printf("gemtek_dsd: %s volume rebuilt from the DSD (%d bytes)\n",
		       FACTORY_VOL, FACTORY_LEN);

out:
	if (ret)
		printf("gemtek_dsd: %s volume left as it was (%d)\n",
		       FACTORY_VOL, ret);
	free(want);
	free(have);

	return ret ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

static int do_ethaddr(void)
{
	u8 lan[ARP_HLEN], wan[ARP_HLEN];

	if (dsd_macs(lan, wan))
		return CMD_RET_FAILURE;

	if (eth_env_set_enetaddr("ethaddr", lan) ||
	    eth_env_set_enetaddr("eth1addr", wan))
		return CMD_RET_FAILURE;

	printf("gemtek_dsd: ethaddr %pM, eth1addr %pM\n", lan, wan);

	return CMD_RET_SUCCESS;
}

static int do_gemtek_dsd(struct cmd_tbl *cmdtp, int flag, int argc,
			 char *const argv[])
{
	if (argc != 2)
		return CMD_RET_USAGE;
	if (!strcmp(argv[1], "sync"))
		return do_sync();
	if (!strcmp(argv[1], "ethaddr"))
		return do_ethaddr();

	return CMD_RET_USAGE;
}

U_BOOT_CMD(gemtek_dsd, 2, 0, do_gemtek_dsd,
	   "factory data from the vendor DSD area",
	   "sync    - create or refresh the factory UBI volume from the DSD\n"
	   "gemtek_dsd ethaddr - set ethaddr / eth1addr from the DSD");
