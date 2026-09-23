/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Entry of the chainloader prefix shim; see shim.c.  The vendor bootm jumps
 * here with MMU and D-cache off.  Clear .bss, take a stack of our own, turn
 * the I-cache on for the unpacking and off again before handing over --
 * the state the vendor bootm would have left for U-Boot on the FIT path.
 */

	.section .text.start, "ax"
	.globl	_start
_start:
	ldr	x0, =__bss_start
	ldr	x1, =__bss_end
1:	cmp	x0, x1
	b.hs	2f
	str	xzr, [x0], #8
	b	1b
2:	ldr	x0, =__stack_top
	mov	sp, x0

	mov	x0, #1
	bl	icache
	bl	shim_main
	mov	x19, x0
	mov	x0, #0
	bl	icache
	cbz	x19, 3f
	mov	x0, xzr
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	br	x19
3:	wfe
	b	3b

/* x0 != 0 sets SCTLR_ELx.I at the current EL, 0 clears it; both invalidate. */
icache:
	mrs	x1, CurrentEL
	ubfx	x1, x1, #2, #2
	cmp	x1, #3
	b.eq	4f
	cmp	x1, #2
	b.eq	5f
	mrs	x2, sctlr_el1
	bic	x2, x2, #(1 << 12)
	cbz	x0, 6f
	orr	x2, x2, #(1 << 12)
6:	msr	sctlr_el1, x2
	b	9f
5:	mrs	x2, sctlr_el2
	bic	x2, x2, #(1 << 12)
	cbz	x0, 7f
	orr	x2, x2, #(1 << 12)
7:	msr	sctlr_el2, x2
	b	9f
4:	mrs	x2, sctlr_el3
	bic	x2, x2, #(1 << 12)
	cbz	x0, 8f
	orr	x2, x2, #(1 << 12)
8:	msr	sctlr_el3, x2
9:	ic	iallu
	dsb	sy
	isb
	ret

	.ltorg
