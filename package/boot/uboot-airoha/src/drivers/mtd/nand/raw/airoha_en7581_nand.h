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

	u8 command;
	u16 short_io_len;
	u16 short_io_pos;
};

/* for SPL */
void airoha_nfc_spl_init(struct airoha_nfc *nfc);
int airoha_nfc_spl_post_init(struct airoha_nfc *nfc);

#endif /* _AIROHA_EN7581_NAND_H_ */
