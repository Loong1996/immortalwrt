// SPDX-License-Identifier: GPL-2.0+
/*
 * Prefix shim of a chainloader slot.
 *
 * A locked Gemtek AN7581 board boots whatever sits in its chainloader
 * partition (flash 0x600000) the way the vendor U-Boot boots a kernel:
 *
 *	flash read 0x602100 <len> $loadaddr; bootm
 *
 * That is the stock command, and it reads the FIT at slot offset 0x2100 --
 * the vendor's own images keep a 0x2100-byte header in front.  The FIT
 * carries this U-Boot LZMA-compressed as its "kernel", so the vendor U-Boot
 * unpacks it to 0x80200000 and jumps there.  That path needs nothing here.
 *
 * Boards that went through an older OpenWrt installer read from 0x600000
 * instead (flash read 0x600000 ...; bootm), and bootm then finds whatever
 * sits at the very start of the slot.  So the slot starts with a legacy
 * uImage holding this shim: the vendor U-Boot copies it to its load address
 * and jumps to it, and it does what the FIT path would have done -- find the
 * LZMA U-Boot inside the FIT behind it, unpack it to 0x80200000, jump.
 * The other recovery U-Boot for this board also insists on a legacy image
 * at offset 0 before it will write a slot, which is the second reason the
 * header is there.
 *
 * The vendor bootm hands over the way the arm64 Linux boot protocol wants
 * it, MMU and data cache off.  Every data access here is therefore a Device
 * access: slow, and faulting when unaligned (hence -mstrict-align).  The
 * instruction cache is switched on around the unpacking, which is what keeps
 * it at a couple of seconds rather than a minute; it is off again, and
 * clean, before the jump.
 *
 * Nothing is known about the slot until the image is built, so the block
 * below is filled in afterwards by mkslot.py, found by its magic.
 */

#include <stddef.h>
#include <stdint.h>
#include "LzmaDec.h"

struct shim_param {
	char		magic[8];	/* "XRSLOT01" */
	uint64_t	link;		/* where this shim runs: the uImage load address */
	uint32_t	base;		/* where the vendor U-Boot read the slot to */
	uint32_t	off;		/* the LZMA U-Boot, from the slot start */
	uint32_t	len;
	uint32_t	crc;		/* crc32 of those len bytes */
	uint32_t	dest;		/* where it runs: the FIT's load address */
	uint32_t	room;		/* how much may be written there */
	uint32_t	uart;		/* 16550 with reg-shift 2; 0 for none */
	uint32_t	pad;
};

extern char _start[];

/*
 * volatile: mkslot.py rewrites these bytes after the link, so the values the
 * compiler sees are not the ones that will be there.
 */
volatile struct shim_param shim_param __attribute__((section(".param"))) = {
	.magic	= { 'X', 'R', 'S', 'L', 'O', 'T', '0', '1' },
	.link	= (uint64_t)(uintptr_t)_start,
	.base	= 0x81800000,
	.dest	= 0x80200000,
	.room	= 0x800000,
	.uart	= 0x1fbf0000,
};

#define UART_LSR	(5 << 2)
#define LSR_THRE	0x20

static void putch(char c)
{
	volatile uint32_t *u = (volatile uint32_t *)(uintptr_t)shim_param.uart;
	int spin = 100000;

	if (!u)
		return;
	while (!(u[UART_LSR / 4] & LSR_THRE) && --spin)
		;
	u[0] = (unsigned char)c;
}

static void puts_(const char *s)
{
	while (*s) {
		if (*s == '\n')
			putch('\r');
		putch(*s++);
	}
}

static void puthex(uint32_t v)
{
	int i;

	puts_("0x");
	for (i = 28; i >= 0; i -= 4)
		putch("0123456789abcdef"[(v >> i) & 0xf]);
}

/* No byte of this may be read as anything wider: Device memory. */
static uint32_t be32(const volatile uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
	uint32_t c = 0xffffffff;
	int k;

	while (n--) {
		c ^= *p++;
		for (k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xedb88320 & -(c & 1));
	}

	return ~c;
}

/* LzmaDec.c wants these; gcc may also emit calls to them. */
void *memcpy(void *d, const void *s, size_t n)
{
	uint8_t *dp = d;
	const uint8_t *sp = s;

	while (n--)
		*dp++ = *sp++;

	return d;
}

void *memset(void *d, int c, size_t n)
{
	uint8_t *dp = d;

	while (n--)
		*dp++ = c;

	return d;
}

/* The decoder allocates its probability table once; nothing else. */
static uint64_t probs[0x10000 / 8];

static void *palloc(ISzAllocPtr p, size_t size)
{
	(void)p;

	return size <= sizeof(probs) ? probs : NULL;
}

static void pfree(ISzAllocPtr p, void *a)
{
	(void)p;
	(void)a;
}

static const ISzAlloc alloc = { palloc, pfree };

static uintptr_t fail(const char *why, uint32_t v)
{
	puts_(why);
	puthex(v);
	puts_("\nThe slot cannot be started this way. From the vendor U-Boot prompt:\n"
	      "  setenv bootcmd 'flash read 0x602100 0x100000 $loadaddr; bootm'; saveenv\n");

	return 0;
}

uintptr_t shim_main(void)
{
	const uint8_t *slot = (const uint8_t *)(uintptr_t)shim_param.base;
	const uint8_t *src = slot + shim_param.off;
	uint8_t *dest = (uint8_t *)(uintptr_t)shim_param.dest;
	uint64_t size = 0;
	SizeT dlen, slen;
	ELzmaStatus st;
	SRes ret;
	int i;

	puts_("\nChainloader prefix shim: slot at ");
	puthex(shim_param.base);
	puts_("\n");

	if (be32(slot) != 0x27051956)
		return fail("No uImage header at the slot address, found ",
			    be32(slot));
	if (be32(slot + 0x2100) != 0xd00dfeed)
		return fail("No FIT at slot + 0x2100, found ",
			    be32(slot + 0x2100));
	if (shim_param.len <= 13)
		return fail("Shim parameters were never filled in: len ",
			    shim_param.len);
	if (crc32(src, shim_param.len) != shim_param.crc)
		return fail("U-Boot image does not match its crc32 ",
			    shim_param.crc);

	/* The .lzma header: 5 bytes of properties, then the size, LE64. */
	for (i = 12; i >= 5; i--)
		size = size << 8 | src[i];
	/* All ones means "not recorded": the stream ends with a marker. */
	if (size != ~0ULL && size > shim_param.room)
		return fail("U-Boot too large to unpack: ", (uint32_t)size);

	puts_("Unpacking U-Boot to ");
	puthex(shim_param.dest);
	puts_(" ...\n");

	dlen = size == ~0ULL ? shim_param.room : size;
	slen = shim_param.len - 13;
	ret = LzmaDecode(dest, &dlen, src + 13, &slen, src, LZMA_PROPS_SIZE,
			 size == ~0ULL ? LZMA_FINISH_ANY : LZMA_FINISH_END,
			 &st, &alloc);
	if (ret != SZ_OK)
		return fail("LZMA error ", ret);
	if (size == ~0ULL ? st != LZMA_STATUS_FINISHED_WITH_MARK : dlen != size)
		return fail("LZMA stream ended early at ", (uint32_t)dlen);

	puts_("Starting U-Boot\n");

	return (uintptr_t)dest;
}
