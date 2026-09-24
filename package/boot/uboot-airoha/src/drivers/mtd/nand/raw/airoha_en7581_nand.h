/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2022 MediaTek Inc. All rights reserved.
 *
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 */

#ifndef _AIROHA_EN7581_NAND_H_
#define _AIROHA_EN7581_NAND_H_

#include <linux/types.h>
#include <linux/mtd/mtd.h>
#include <linux/compiler.h>
#include <linux/mtd/rawnand.h>

struct airoha_nfc {
	struct nand_chip nand;

	void __iomem *nfi_regs;
	void __iomem *ecc_regs;

	u32 spare_per_sector;
	/* CNFG_ECC_DATA_SOURCE_INV for the active page format */
	bool data_inv;

	u8 command;
	u16 short_io_len;
	u16 short_io_pos;
};

/*
 * Another page format than the one this driver writes: the stock firmware's,
 * so an image dumped from the stock system without OOB can be written back in
 * a form the stock loader reads.  ecc is the BCH strength per 512 bytes, spare
 * the bytes that follow each 512, fdm the FDM bytes among them and fecc how
 * many of those the codeword covers; inv inverts the data before encoding and
 * swap trades the byte at the physical bad-block position (first OOB column)
 * with FDM0, the way MediaTek's NFI drivers do.
 */
struct airoha_nand_fmt {
	u8 ecc;
	u8 spare;
	u8 inv;
	u8 fdm;
	u8 fecc;
	u8 swap;
};

/*
 * All take the flash as a whole (a partition is resolved to its master) and a
 * page-aligned offset, and leave the controller in its own format afterwards.
 * The *_phys pair moves writesize + oobsize bytes in the chip's own byte
 * order, with no ECC at all -- what a programmer reads off the chip.
 */
bool airoha_nand_is(struct mtd_info *mtd);
void airoha_nand_get_fmt(struct mtd_info *mtd, struct airoha_nand_fmt *f);
int airoha_nand_check_fmt(struct mtd_info *mtd, const struct airoha_nand_fmt *f);
int airoha_nand_read_phys(struct mtd_info *mtd, loff_t ofs, u8 *buf);
int airoha_nand_write_phys(struct mtd_info *mtd, loff_t ofs, const u8 *buf);
int airoha_nand_read_as(struct mtd_info *mtd, loff_t ofs,
			const struct airoha_nand_fmt *f, u8 *data,
			unsigned int *flips);
int airoha_nand_write_as(struct mtd_info *mtd, loff_t ofs,
			 const struct airoha_nand_fmt *f, const u8 *data);

/* for SPL */
void airoha_nfc_spl_init(struct airoha_nfc *nfc);
int airoha_nfc_spl_post_init(struct airoha_nfc *nfc);

#endif /* _AIROHA_EN7581_NAND_H_ */
