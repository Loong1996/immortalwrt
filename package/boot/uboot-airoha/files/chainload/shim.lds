/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Linked where the uImage header tells the vendor bootm to copy it: clear of
 * the vendor ATF below 0x80200000, of U-Boot unpacked at 0x80200000, and of
 * the slot read to 0x81800000.
 */
OUTPUT_ARCH(aarch64)
ENTRY(_start)
SECTIONS
{
	. = 0x81000000;
	.text : { *(.text.start) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { . = ALIGN(8); KEEP(*(.param)) *(.data*) }
	.bss (NOLOAD) : {
		. = ALIGN(8);
		__bss_start = .;
		*(.bss*) *(COMMON)
		. = ALIGN(8);
		__bss_end = .;
	}
	. = ALIGN(16) + 0x4000;
	__stack_top = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.interp) *(.dynamic) }
}
