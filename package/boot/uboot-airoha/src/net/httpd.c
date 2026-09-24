// SPDX-License-Identifier: GPL-2.0+
/*
 * Airoha Web U-Boot -- HTTP recovery server.
 *
 * Serves a recovery page on GET and accepts one or more images via
 * multipart/form-data on POST.  Each part is flashed where the TCP stream
 * left it, and the flashing itself is delegated to the board's environment
 * scripts (with a built-in fallback for each), so no flash logic is
 * duplicated here.
 *
 * What the page offers, one task per sidebar entry:
 *
 *   routine flash    "firmware" into the fit volume
 *   bootloader       "bl2" and/or "fip", optionally with the firmware and
 *                    a rebuild of the whole ubi partition ("format")
 *   back to stock    "stock" written to raw flash at "stockoff"
 *                    (CONFIG_CMD_HTTPD_STOCK_RESTORE)
 *   UBI volumes      per-unit factory data volumes named in
 *                    CONFIG_HTTPD_FACTORY_VOLS ("fvol_<name>"), or any
 *                    volume by name ("ubivol" + "ubifile"), created when it
 *                    does not exist yet; "stay" keeps the server up after
 *                    the write instead of rebooting
 *   device details   GET /info, JSON read at request time from the device
 *                    tree, MTD and UBI -- the page itself carries no board
 *                    name, so one build of it serves every board
 *   health check     GET /check (BL2, bad blocks, the UBI volumes the boot
 *                    needs, factory volumes, U-Boot MAC)
 *   serial log       GET /log, the console output recorded since power-on
 *                    (CONFIG_CONSOLE_RECORD)
 *   backup           GET /dump?vol=<name> or ?off=&len=, read out to the
 *                    browser as a file -- the only way to keep the
 *                    per-unit factory volumes when the system is gone
 *   network          GET /net for the address and the link state of each
 *                    port (polled while that tab is open); GET
 *                    /netmode?mode=server|static|client&ip=&mask=&save= to
 *                    change it; GET /dhcpgw?on=0|1 for whether a lease
 *                    carries this board as the gateway
 *   environment      GET /env read-only, GET /envreset for the defaults
 *   settings         GET /wipecfg removes rootfs_data: OpenWrt's settings
 *                    and packages, gone on the next boot
 *   reboot           GET /reboot
 *
 * The page also polls GET /ping.  That answer is the only thing telling
 * it whether the board is still there: writes, whole-chip reads and the
 * reboot all take the server away for a while, and a page that still
 * looks operable is worse than one that says it is not.
 *
 * The page lives in files/httpd/page.html next to this package; gen.py
 * splices it in between the PAGE_BEGIN / PAGE_END markers below.  Edit the
 * HTML, rerun the script, never the string literal by hand.
 *
 * Built on U-Boot's own TCP stack (net/tcp.c), modelled on net/fastboot_tcp.c.
 * No third-party IP stack is pulled in, so tftpboot / dhcp / bootmenu keep
 * working unchanged.
 */

#include <command.h>
#include <console.h>
#include <cyclic.h>
#include <dm.h>
#include <env.h>
#include <env_internal.h>
#include <led.h>
#include <miiphy.h>
#include <mtd.h>
#include <net.h>
#include <net/tcp.h>
#include <search.h>
#include <stdarg.h>
#include <time.h>
#include <ubi_uboot.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/libfdt.h>
#include <linux/mtd/mtd.h>
#include <membuf.h>
#include <linux/string.h>
#include <asm/global_data.h>
#include <asm/unaligned.h>
#include <version_string.h>
#include <linux/sizes.h>
/*
 * <ubi_uboot.h> above pulls in <linux/crc32.h>, which #defines crc32() to
 * crc32_le().  A function-like macro cannot tell a declaration from a call,
 * so it also eats the crc32() prototype in the header below -- that is the
 * compile error, and it is the harmless half.
 *
 * The quiet half is that crc32_le() (UBI carries its own in
 * drivers/mtd/ubi/crc32.c) is the raw Linux LFSR, inverted at neither end.
 * It would answer a plausible-looking number that never matches what
 * "crc32 file.bin" prints on the user's machine -- and being compared
 * against exactly that is the only reason /dumpinfo reports one.  UBI gets
 * away with it because it only ever compares against its own stored values.
 *
 * So drop the macro: the declaration and every call here then mean
 * lib/crc32.c's crc32(), which inverts at both ends and, as its own comment
 * promises, chains from a previous result -- what /dump needs per window.
 */
#undef crc32
#include <u-boot/crc.h>
#include <vsprintf.h>
#include "../drivers/mtd/ubi/ubi.h"

DECLARE_GLOBAL_DATA_PTR;

#define HTTPD_PORT		80

/* Version of the recovery page itself, shown in the sidebar and on the console. */
#define WEB_VERSION		"1.0.1"

#define AUTHOR			"Loong"
#define AUTHOR_HOST		"github.com/Loong1996"
#define AUTHOR_URL		"https://" AUTHOR_HOST
#define PROJECT_HOST		"github.com/Loong1996/ImmortalWrt-Airoha"
#define PROJECT_URL		"https://" PROJECT_HOST
#define PORTAL_HOST		"loong1996.github.io/ImmortalWrt-Airoha"
#define PORTAL_URL		"https://" PORTAL_HOST "/"

/* Enough for the request line + headers of any sane browser. */
#define HDRBUF_SZ		2048

/* tcp->priv values: per-connection classification. */
#define CONN_UNKNOWN		((void *)0)
#define CONN_GET		((void *)1)
#define CONN_POST		((void *)2)
#define CONN_INFO		((void *)3)
#define CONN_CHECK		((void *)4)
#define CONN_LOG		((void *)5)
#define CONN_PING		((void *)6)
#define CONN_ENV		((void *)7)
#define CONN_ENVRESET		((void *)8)
#define CONN_REBOOT		((void *)9)
#define CONN_DUMP		((void *)10)
#define CONN_DUMPBUSY		((void *)11)
#define CONN_DUMPINFO		((void *)12)
#define CONN_POSTBUSY		((void *)13)
#define CONN_STOCK		((void *)14)
#define CONN_SCAN		((void *)15)
#define CONN_BOOT		((void *)16)
#define CONN_BOOTONCE		((void *)17)
#define CONN_NETMODE		((void *)18)
#define CONN_WR			((void *)19)
#define CONN_WRBUSY		((void *)20)
#define CONN_NET		((void *)21)
/*
 * Answered and done with, but the peer is still sending: swallow the rest
 * of the request and say nothing.  An upload is refused long before the
 * browser has finished pushing the image, and the bytes still on the wire
 * have to be taken off it -- see httpd_on_snd_una_update().
 */
#define CONN_DRAIN		((void *)22)
#define CONN_WIPECFG		((void *)23)
#define CONN_DHCPGW		((void *)24)

/*
 * Commands run after the response has been flushed and net_loop() returned.
 * Keeping them in the environment means a user can inspect and override the
 * flashing steps from the U-Boot console without rebuilding.
 *
 * Each one has a built-in equivalent, because the saved environment is itself
 * something recovery has to survive: a board carrying an older ubootenv
 * volume shadows the defaults compiled into this image, and would fail at the
 * first step -- precisely when there is no other way in.
 */
#define ENV_WRITE_BL2		"web_uboot_write_bl2"
#define ENV_FORMAT_UBI		"web_uboot_format_ubi"
#define ENV_WRITE_FIP		"web_uboot_write_fip"
#define ENV_WRITE_FIT		"ubi_write_production"
#define ENV_VER			"web_uboot_envver"

#define UBI_PART		"ubi"
#define CMD_ATTACH_UBI		"ubi part " UBI_PART

#define DEF_WRITE_BL2		"mtd erase bl2 && " \
				"mtd write bl2 $loadaddr 0x800 $filesize"
#define DEF_FORMAT_UBI		"ubi detach ; mtd erase " UBI_PART " && " \
				CMD_ATTACH_UBI
/*
 * A routine bootloader update must not touch rootfs_data (the overlay
 * holding every OpenWrt setting), and does not need to: fip is always
 * created at the same fixed 0x100000, so an in-place "ubi write" on an
 * already-existing fip fits inside its own current reservation with
 * nothing to free.
 *
 * ubi_write_fip -- the board's own script, used by the bootmenu entry
 * immediately followed by reset_factory -- is only reached here when fip
 * does not exist yet (first migration, before rootfs_data exists either).
 * That is the one case where its unconditional "evict rootfs_data, then
 * create" is both necessary and harmless, so it is reused rather than
 * duplicated.
 */
#define DEF_WRITE_FIP		"if ubi check fip ; then " \
				"ubi write $loadaddr fip $filesize ; " \
				"else run ubi_write_fip ; fi"
/*
 * Clearing OpenWrt's settings is removing its overlay.  The next normal boot
 * runs ubi_prepare_rootfs, which creates an empty rootfs_data again, and UBIFS
 * formats an empty volume on its first mount -- the same thing a flash does
 * to it.  The board's ubi_remove_rootfs is used when it is there so the
 * serial menu and this page take the same path.
 */
#define ENV_REMOVE_ROOTFS	"ubi_remove_rootfs"
#define DEF_REMOVE_ROOTFS	"ubi check rootfs_data && ubi remove rootfs_data"
#define DEF_WRITE_FIT		"ubi check fit && ubi remove fit ; " \
				"ubi check rootfs_data && ubi remove rootfs_data ; " \
				"ubi create fit $filesize dynamic && " \
				"ubi write $loadaddr fit $filesize"

/* UBI's own limit is 127; this bounds the on-stack copies. */
#define UBIVOL_NAME_MAX		64

/* Form field names. */
#define FIELD_BL2		"bl2"
#define FIELD_FIP		"fip"
#define FIELD_FIT		"firmware"
#define FIELD_FORMAT		"format"
#define FIELD_STOCK		"stock"
#define FIELD_STOCK_OFF		"stockoff"
#define FIELD_FVOL_PREFIX	"fvol_"
#define FIELD_UBIVOL_NAME	"ubivol"
#define FIELD_UBIVOL_FILE	"ubifile"
#define FIELD_TRYBOOT		"tryboot"

#define MAX_PARTS		12
#define MAX_FVOLS		8

/*
 * Parts are flashed in place, so their start address is wherever the form
 * data happened to land.  Align it down for the NAND layer; a multipart part
 * header is never shorter than ~90 bytes, so the bytes stepped on belong to
 * that header and never to the previous part's payload.
 */
#define FLASH_ALIGN		64

/*
 * ---- the page -------------------------------------------------------------
 *
 * Generated from files/httpd/page.html by files/httpd/gen.py.  One board
 * name does not appear anywhere in it: whatever is board-specific reaches
 * the browser through GET /info at request time.
 */
static const char resp_form[] =
	"HTTP/1.0 200 OK\r\n"
	"Content-Type: text/html; charset=utf-8\r\n"
	"Connection: close\r\n"
	"\r\n"
/* @@PAGE_BEGIN@@ */
	"<!DOCTYPE html><html lang=zh data-t=light><head><meta charset=utf-8>\n"
	"<script>try{var t=localStorage.getItem('xgtheme'),g=localStorage.getItem('xglang')||(/^zh/i.test"
		"(navigator.language||'')?'zh':'en');if(t)document.documentElement.setAttribute('data-t',t);if(g="
		"='en')document.documentElement.setAttribute('lang','en')}catch(e){}</script>\n"
	"<meta name=viewport content=\"width=device-width,initial-scale=1\">\n"
	"<title>Airoha Web U-Boot</title>\n"
	"<style>\n"
	":root{--bg:#f5f5f7;--side:#ececef;--card:#fff;--fg:#1d1d1f;--c2:#6e6e73;--c3:#aeaeb2;--sep:rgba("
		"60,60,67,.14);--blue:#0a66d6;--red:#d9342b;--org:#b8500f;--grn:#1f8a3b;--fill:#e8e8ed;--segon:#f"
		"ff;--btn:#fff;--btnb:rgba(0,0,0,.14)}\n"
	"[data-t=dark]{--bg:#1e1e20;--side:#242426;--card:#2a2a2d;--fg:#f5f5f7;--c2:#a1a1a6;--c3:#6e6e73;"
		"--sep:rgba(255,255,255,.1);--blue:#2f7ff2;--red:#ff5f57;--org:#f0a04a;--grn:#4cd964;--fill:#3a3a"
		"3d;--segon:#57575c;--btn:#3a3a3d;--btnb:rgba(255,255,255,.14)}\n"
	"*{box-sizing:border-box}\n"
	"[hidden]{display:none!important}\n"
	"html{height:100%;max-width:100%;overflow-x:hidden}\n"
	"body{height:100%;max-width:100%;margin:0;background:var(--bg);color:var(--fg);font:13px/1.5"
		" -apple-system,BlinkMacSystemFont,\"SF Pro Text\",\"PingFang SC\",\"Microsoft YaHei\",\"Helvetica"
		" Neue\",sans-serif}\n"
	"a{color:var(--blue);text-decoration:none}a:hover{text-decoration:underline}\n"
	"svg{width:1em;height:1em;vertical-align:-.15em;fill:none;stroke:currentColor;stroke-width:2;stro"
		"ke-linecap:round;stroke-linejoin:round}\n"
	".app{display:grid;grid-template-columns:minmax(9.5rem,12.5rem)"
		" minmax(0,1fr);min-height:100%;width:100%;max-width:100%;background:var(--bg)}\n"
	"input[type=file]{position:absolute;width:1px;height:1px;opacity:0;pointer-events:none}\n"
	".pb{border:1px solid var(--btnb);background:var(--btn);color:var(--fg);border-radius:6px;font:in"
		"herit;padding:.22rem .8rem;cursor:pointer;box-shadow:0 .5px 1px"
		" rgba(0,0,0,.08);white-space:nowrap}\n"
	".pb:active{filter:brightness(.94)}\n"
	".pb:focus-visible,.nav:focus-visible,.tb:focus-visible{outline:0;box-shadow:0 0 0 3px"
		" rgba(10,102,214,.35)}\n"
	".pb.pri{background:var(--blue);color:#fff;border-color:transparent;font-weight:500}\n"
	".pb:disabled{opacity:.5;cursor:default}\n"
	".pb.red{background:var(--red);color:#fff;border-color:transparent;font-weight:500}\n"
	".fn{color:var(--c2);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}\n"
	".fn.has{color:var(--fg)}\n"
	".fs{color:var(--c3);font-variant-numeric:tabular-nums;white-space:nowrap;font-size:.92em}\n"
	".sw{appearance:none;-webkit-appearance:none;position:relative;width:26px;height:15px;margin:0;bo"
		"rder-radius:99px;background:var(--fill);flex:none;cursor:pointer;transition:background"
		" .2s;box-shadow:inset 0 0 0 1px var(--sep);outline:0}\n"
	".sw::after{content:\"\";position:absolute;top:1px;left:1px;width:13px;height:13px;border-radius:50"
		"%;background:#fff;box-shadow:0 1px 2px rgba(0,0,0,.3);transition:transform .2s}\n"
	".sw:checked{background:var(--blue);box-shadow:none}.sw:checked::after{transform:translateX(11px)"
		"}\n"
	".sw:focus-visible{box-shadow:0 0 0 3px rgba(10,102,214,.35)}\n"
	".mono{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:.92em}\n"
	".box{background:var(--card);border:1px solid var(--sep);border-radius:9px;overflow:hidden}\n"
	".bh{font-weight:600;margin:1.1rem 0 .4rem;font-size:.95rem}\n"
	".bh small{color:var(--c2);font-weight:400;margin-left:.5rem}\n"
	".fr{display:grid;grid-template-columns:150px minmax(0,1fr);gap:.8rem;align-items:center;padding:"
		".55rem .9rem;position:relative;min-height:40px}\n"
	".fr+.fr::before{content:\"\";position:absolute;left:.9rem;right:0;top:0;border-top:1px solid"
		" var(--sep)}\n"
	".fr.wide{grid-template-columns:minmax(0,1fr) auto}\n"
	".fl{font-weight:500}\n"
	".fl small{display:block;color:var(--c2);font-weight:400;font-size:.88em;line-height:1.3}\n"
	".fc{display:flex;align-items:center;gap:.6rem;min-width:0}\n"
	".fc.end{justify-content:flex-end}\n"
	"input[type=text]{border:1px solid var(--btnb);background:var(--card);color:var(--fg);border-radi"
		"us:5px;font:inherit;padding:.22rem .5rem;width:100%;max-width:220px;outline:0}\n"
	"input[type=text]:focus,.fc select:focus{box-shadow:0 0 0 3px"
		" rgba(10,102,214,.25);border-color:var(--blue)}\n"
	".fc select{border:1px solid var(--btnb);background:var(--card);color:var(--fg);border-radius:5px"
		";font:inherit;padding:.22rem .5rem;outline:0;max-width:220px}\n"
	"input::placeholder{color:var(--c3)}\n"
	".note{color:var(--c2);font-size:.92em;line-height:1.45;margin:.45rem .2rem 0}\n"
	".note b{color:var(--org);font-weight:500}\n"
	".act{display:flex;align-items:center;justify-content:flex-end;gap:.8rem;margin-top:auto;padding-"
		"top:1.4rem}\n"
	".st{color:var(--c2);margin-right:auto;font-variant-numeric:tabular-nums;min-width:0;overflow:hid"
		"den;text-overflow:ellipsis;white-space:nowrap}\n"
	"@media (min-width:701px){.side{position:sticky;top:0;align-self:start;height:100vh;overflow-y:au"
		"to;scrollbar-width:none}\n"
	".side::-webkit-scrollbar{display:none}}\n"
	".side{background:var(--side);border-right:1px solid var(--sep);padding:.6rem .6rem"
		" 1rem;display:flex;flex-direction:column}\n"
	".id{padding:.5rem .6rem 1rem}\n"
	".id b{display:block;font-size:.98rem}\n"
	".id small{color:var(--c2)}\n"
	".id small a,.about span a{color:inherit}\n"
	".id small a:hover,.about span a:hover{color:var(--blue)}\n"
	".foot{margin-top:auto;display:flex;align-items:center;justify-content:space-between;gap:.5rem"
		" .45rem;padding:.6rem .4rem 0}\n"
	".foot .tb{margin-top:0;align-self:auto}\n"
	".fbtns{display:flex;align-items:center;gap:.3rem}\n"
	".lg{display:inline-flex;align-items:center;height:26px;padding:2px;gap:2px;border-radius:99px;ba"
		"ckground:var(--fill);box-shadow:inset 0 0 0 1px var(--sep);flex:none}\n"
	".lg button{appearance:none;-webkit-appearance:none;border:0;background:transparent;color:var(--c"
		"2);font:inherit;font-size:.86em;font-weight:500;line-height:1;padding:0"
		" .42rem;height:100%;border-radius:99px;cursor:pointer;outline:0;display:inline-flex;align-items:"
		"center;transition:background .18s,color .18s,box-shadow .18s}\n"
	".lg button:hover{color:var(--fg)}\n"
	".lg button[aria-checked=true]{background:var(--segon);color:var(--fg);box-shadow:0 1px 2px"
		" rgba(0,0,0,.14),0 0 0 .5px rgba(0,0,0,.04)}\n"
	".lg button:focus-visible{box-shadow:0 0 0 3px rgba(10,102,214,.35)}\n"
	".live{display:flex;align-items:center;color:var(--c2);font-size:.92em;white-space:nowrap;min-wid"
		"th:0;overflow:hidden;text-overflow:ellipsis}\n"
	".alert .mi.bad{background:var(--red)}\n"
	".alert .mi.info{background:var(--blue)}\n"
	".alert .mi.ok{background:var(--grn)}\n"
	".alert .hint{color:var(--c2);font-size:.9em;margin-top:.7rem}\n"
	".nav{border:0;background:transparent;color:var(--fg);font:inherit;width:100%;text-align:left;dis"
		"play:flex;align-items:center;gap:.55rem;padding:.35rem .5rem;border-radius:6px;cursor:pointer}\n"
	".nav .ic{width:22px;height:22px;border-radius:6px;display:inline-flex;align-items:center;justify"
		"-content:center;color:#fff;flex:none}\n"
	".nav .ic svg{width:13px;height:13px}\n"
	".n1 .ic{background:#0a84ff}.n2 .ic{background:#5e5ce6}.n3 .ic{background:#8e8e93}.n4"
		" .ic{background:#ff9f0a}.n5 .ic{background:#64748b}.n6 .ic{background:#af52de}.n7"
		" .ic{background:#30d158}.n8 .ic{background:#5ac8fa}.n9 .ic{background:#00b4a0}.n10"
		" .ic{background:#a2845e}.n11 .ic{background:#ff453a}\n"
	".nav:hover{background:var(--sep)}\n"
	".nav[aria-current],.nav[aria-current]:hover{background:rgba(10,102,214,.12);color:var(--blue);fo"
		"nt-weight:500}\n"
	"[data-t=dark] .nav[aria-current],[data-t=dark] .nav[aria-current]:hover{background:rgba(47,127,2"
		"42,.26);color:#fff}\n"
	"[data-busy] .nav{opacity:.45;pointer-events:none}\n"
	".nsep{height:1px;background:var(--sep);margin:.45rem .5rem}\n"
	".tb{margin-top:auto;align-self:flex-start;border:1px solid"
		" var(--btnb);background:transparent;color:var(--c2);border-radius:50%;padding:0;width:26px;heigh"
		"t:26px;font:inherit;cursor:pointer;display:inline-flex;align-items:center;justify-content:center"
		"}\n"
	".tb svg{width:14px;height:14px}\n"
	".tb:hover{color:var(--fg)}\n"
	".pane{display:none;padding:1.4rem 1.6rem 1.4rem;min-width:0}\n"
	".pane[data-on]{display:flex;flex-direction:column;min-height:0;flex:1}\n"
	"h1{font-size:1.45rem;font-weight:700;margin:0 0 .15rem;letter-spacing:-.01em}\n"
	".sub{color:var(--c2);margin:0 0 1.1rem}\n"
	".drop{display:flex;align-items:center;gap:1rem;background:var(--card);border:1.5px dashed"
		" var(--btnb);border-radius:9px;padding:1rem 1.1rem;cursor:pointer}\n"
	".drop:hover,.drop.has,.drop.over{border-color:var(--blue);border-style:solid}\n"
	".fr.over{background:var(--fill)}\n"
	":root{--drop:\"松手放到这里\"}\n"
	"[lang=en]{--drop:\"Drop it here\"}\n"
	".fr.over::after{content:var(--drop);position:absolute;right:.9rem;color:var(--blue);font-size:.9"
		"em}\n"
	".drop .ic{width:40px;height:40px;border-radius:9px;background:var(--fill);color:var(--blue);disp"
		"lay:inline-flex;align-items:center;justify-content:center;flex:none}\n"
	".drop .ic svg{width:20px;height:20px}\n"
	".drop .tx{min-width:0;flex:1}\n"
	".drop .t{font-weight:600;font-size:.98rem;display:flex;min-width:0}\n"
	".drop .t .fn{display:block}\n"
	".drop .s{color:var(--c2)}\n"
	".prog{margin-top:auto;padding-top:1.4rem}\n"
	".pw{height:6px;border-radius:99px;background:var(--fill);overflow:hidden}\n"
	".pbar{height:100%;width:0;background:var(--blue);border-radius:99px;transition:width .2s}\n"
	".pbar.ind{width:100%;animation:pu 1.4s ease-in-out infinite}\n"
	"@keyframes pu{0%,100%{opacity:.4}50%{opacity:1}}\n"
	".ps{display:flex;justify-content:space-between;color:var(--c2);margin-top:.4rem;font-variant-num"
		"eric:tabular-nums}\n"
	".ps span{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}\n"
	".ps .pct{flex:none;margin-left:1rem;max-width:60%}\n"
	".prog.ok .pbar{background:var(--grn)}.prog.ok .ps{color:var(--grn)}\n"
	".prog.bad .pbar{background:var(--red)}.prog.bad .ps{color:var(--red);align-items:flex-start}.pro"
		"g.bad .pwhat{white-space:normal;overflow:visible;text-overflow:clip}\n"
	".kv{width:100%;border-collapse:collapse}\n"
	".kv td{padding:.5rem .9rem;vertical-align:top;border-top:1px solid"
		" var(--sep);word-break:break-all}\n"
	".kv tr:first-child td{border-top:0}\n"
	".kv td:first-child{width:150px;color:var(--c2);word-break:normal}\n"
	".kv th{text-align:left;font-weight:500;color:var(--c2);padding:.45rem .9rem;font-size:.92em}\n"
	".kv .n{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}\n"
	".about{display:flex;gap:1rem;align-items:center;background:var(--card);border:1px solid"
		" var(--sep);border-radius:9px;padding:1.1rem 1.2rem}\n"
	".about .logo{width:56px;height:56px;border-radius:14px;background:linear-gradient(145deg,#0a84ff"
		",#5e5ce6);color:#fff;display:flex;align-items:center;justify-content:center;flex:none}\n"
	".about .logo svg{width:30px;height:30px}\n"
	".about b{font-size:1.05rem;display:block}\n"
	".about span{color:var(--c2)}\n"
	".fin{background:var(--card);border:1px solid var(--sep);border-radius:9px;padding:1rem"
		" 1.1rem;display:flex;gap:.9rem;align-items:flex-start}\n"
	".fin .ic{width:36px;height:36px;border-radius:50%;background:var(--grn);color:#fff;display:inlin"
		"e-flex;align-items:center;justify-content:center;flex:none}\n"
	".fin .ic svg{width:20px;height:20px;stroke-width:2.5}\n"
	".fin>div{min-width:0;flex:1}\n"
	".fin>div>b{display:block;font-size:.98rem;margin:.3rem 0 .5rem}\n"
	".lt{border-collapse:collapse;width:100%}\n"
	".lt td{padding:.3rem 0;vertical-align:top;border-top:1px solid var(--sep)}\n"
	".lt td:first-child{width:4rem;color:var(--c2);font-weight:500}\n"
	".lt td b{color:var(--red);font-weight:500}\n"
	".steps{margin:0;padding-left:1.3rem}\n"
	".steps li{padding:.15rem 0}\n"
	".steps b{color:var(--org);font-weight:500}\n"
	".mask{position:fixed;inset:0;background:rgba(0,0,0,.28);display:none;z-index:9;padding:1rem}\n"
	".mask[data-on]{display:flex}\n"
	".alert{margin:auto;width:min(340px,100%);background:var(--card);border-radius:12px;padding:1.2re"
		"m 1.2rem 1rem;box-shadow:0 24px 60px -10px rgba(0,0,0,.5),0 0 0 1px var(--sep)}\n"
	".alert .hd{display:flex;align-items:center;gap:.7rem;margin-bottom:.8rem}\n"
	".alert .mi{width:38px;height:38px;border-radius:9px;background:var(--org);color:#fff;display:inl"
		"ine-flex;align-items:center;justify-content:center;flex:none}\n"
	".alert .mi svg{width:20px;height:20px;stroke-width:2.2}\n"
	".alert h2{font-size:1rem;margin:0}\n"
	".alert h2 small{display:block;color:var(--c2);font-weight:400;font-size:.85rem}\n"
	".r{display:flex;justify-content:space-between;gap:1rem;padding:.3rem 0;font-size:.95em}\n"
	".r+.r{border-top:1px solid var(--sep)}\n"
	".r span:first-child{color:var(--c2)}\n"
	".r .v{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}\n"
	"/* 文件名那一格：.r span:first-child 会连 .vc 里的文件名一起染灰，这里压过它 */\n"
	".r .vc{min-width:0;display:flex;flex-direction:column;align-items:flex-end;text-align:right}\n"
	".r:has(.vc)>span:first-child{flex:none}\n"
	".r .vc .v{max-width:100%;color:var(--fg)}\n"
	".r .vc .md{font-size:.86em;color:var(--c2)}\n"
	".r .vc .ok{color:var(--grn)}.r .vc .bad{color:var(--red)}.r .vc .md.bad{font-weight:500}\n"
	".w{font-size:.9em;color:var(--org);margin-top:.5rem;line-height:1.45}\n"
	".w+.w{margin-top:.3rem}\n"
	".e{font-size:.9em;color:var(--red);margin-top:.5rem;line-height:1.45;font-weight:500}\n"
	".btns{display:flex;justify-content:flex-end;gap:.6rem;margin-top:1rem}\n"
	".demo{color:var(--c3);font-size:.85em;margin-top:1.4rem}\n"
	".ban{margin:1.2rem 1.6rem 0;padding:.65rem .9rem;border-radius:9px;background:rgba(217,52,43,.08"
		");border:1px solid rgba(217,52,43,.35);color:var(--red);font-weight:500;line-height:1.45;display"
		":flex;gap:.6rem;align-items:flex-start}\n"
	".ban svg{flex:none;margin-top:.2em;stroke-width:2.2}\n"
	".ban b{font-weight:600}\n"
	".tag{display:inline-block;margin-left:.45rem;padding:0"
		" .35rem;border-radius:4px;background:var(--fill);color:var(--c2);font-size:.85em;font-weight:400"
		";vertical-align:.05em}\n"
	".dlb{padding:.1rem .6rem;font-size:.92em}\n"
	".kv td.empty{text-align:center;color:var(--c3);padding:1.7rem"
		" .9rem;line-height:1.6;word-break:normal}\n"
	".kv td.empty b{display:block;color:var(--c2);font-weight:500;margin-bottom:.1rem}\n"
	".kv td.b{text-align:right;padding:.3rem .9rem;width:1%}\n"
	".kv th.g{padding-top:.7rem;font-weight:600;color:var(--fg);border-top:1px solid var(--sep)}\n"
	".kv tr:first-child th.g{padding-top:.45rem;border-top:0}\n"
	".seg{display:flex;align-self:flex-start;max-width:100%;overflow-x:auto;background:var(--fill);bo"
		"rder-radius:999px;padding:2px;gap:2px;margin:0 0 1rem;flex:none;scrollbar-width:none}\n"
	".seg::-webkit-scrollbar{display:none}\n"
	".seg button{appearance:none;-webkit-appearance:none;border:0;background:transparent;color:var(--"
		"c2);font:inherit;font-weight:500;padding:.3rem .9rem;border-radius:999px;cursor:pointer;white-sp"
		"ace:nowrap;outline:0;transition:background .18s,color .18s,box-shadow .18s}\n"
	".seg button:hover{color:var(--fg)}\n"
	".seg button[aria-selected=true]{background:var(--segon);color:var(--fg);box-shadow:0 1px 2px"
		" rgba(0,0,0,.14),0 0 0 .5px rgba(0,0,0,.04)}\n"
	".seg button:focus-visible{box-shadow:0 0 0 3px rgba(10,102,214,.35)}\n"
	".sp{display:none;min-width:0}\n"
	".sp[data-on]{display:flex;flex-direction:column;flex:1;min-width:0}\n"
	".sp>.bh:first-child{margin-top:0}\n"
	".bar{display:flex;gap:.6rem;align-items:center;flex-wrap:wrap;margin:0 0 .6rem}\n"
	".bar label{display:flex;gap:.4rem;align-items:center;color:var(--c2)}\n"
	".envv{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:.92em;white-space:pre-wrap;wor"
		"d-break:break-all}\n"
	".dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:.38rem;background:"
		"var(--c3)}\n"
	".dot.s0{background:var(--grn)}.dot.s1{background:var(--org)}.dot.s2{background:var(--red)}\n"
	".kv td.s1{color:var(--org)}.kv td.s2{color:var(--red)}\n"
	".kv td.hot{color:var(--red)}\n"
	".kv td.cmd{color:var(--c2);font-family:ui-monospace,Menlo,Consolas,monospace;font-size:.92em}\n"
	".row{display:flex;gap:.6rem;margin-top:.6rem;justify-content:flex-end;align-items:center}\n"
	".log{margin:0;padding:.7rem .9rem;max-height:min(360px,60vh);overflow:auto;font-family:ui-monosp"
		"ace,Menlo,Consolas,monospace;font-size:.88em;line-height:1.4;white-space:pre-wrap;word-break:bre"
		"ak-all;color:var(--fg)}\n"
	".vbar{display:flex;height:24px;border-radius:6px;overflow:hidden;border:1px solid"
		" var(--sep);background:var(--card);margin:0 0 .5rem}\n"
	".vbar i{display:block;min-width:2px;position:relative}\n"
	".vbar i.re::after{content:\"\";position:absolute;inset:0;background:repeating-linear-gradient(45de"
		"g,rgba(255,255,255,.5) 0 3px,transparent 3px 7px)}\n"
	".vbar i.free{background:var(--fill)}\n"
	".vbar i.rsv{background:var(--c3)}\n"
	".vleg{display:flex;flex-wrap:wrap;gap:.2rem .85rem;margin:0 0"
		" .3rem;color:var(--c2);font-size:.92em}\n"
	".vleg span{display:flex;align-items:center;gap:.35rem;white-space:nowrap}\n"
	".vleg em{width:9px;height:9px;border-radius:2px;font-style:normal;flex:none;position:relative}\n"
	".vleg em.re::after{content:\"\";position:absolute;inset:0;border-radius:2px;background:repeating-l"
		"inear-gradient(45deg,rgba(255,255,255,.55) 0 2px,transparent 2px 4px)}\n"
	".vleg em.free{background:var(--fill);box-shadow:inset 0 0 0 1px var(--sep)}\n"
	"@media (max-width:860px){.fr{grid-template-columns:1fr}input[type=text]{max-width:100%}.kv"
		" td:first-child{width:auto}}\n"
	"@media (max-width:700px){.app{grid-template-columns:1fr}.side{border-right:0;border-bottom:1px"
		" solid var(--sep)}.fr{grid-template-columns:1fr}.tb{margin-top:.4rem}.foot{flex-wrap:wrap}.ban{m"
		"argin:1rem 1rem 0}.pane{padding:1rem}}\n"
	"</style>\n"
	"</head><body><div class=app id=app>\n"
	"<div class=side>\n"
	"<div class=id><b>Airoha Web U-Boot</b><small>"
		WEB_VERSION
		" · <a href=\""
		AUTHOR_URL
		"\" target=_blank rel=noopener title=\""
		AUTHOR_HOST
		"\">"
		AUTHOR
		"</a></small></div>\n"
	"<button class=\"nav n1\" data-p=p1 aria-current=true onclick=nav(this)><span class=ic><svg"
		" viewBox=\"0 0 24 24\"><path d=\"M12 16V4M6 10l6-6 6 6M4 20h16\"/></svg></span>日常刷机</button>\n"
	"<button class=\"nav n2\" data-p=p2 onclick=nav(this)><span class=ic><svg viewBox=\"0 0 24"
		" 24\"><path d=\"M4 6h16M4 12h16M4 18h10\"/></svg></span>引导升级</button>\n"
	"<button class=\"nav n8\" data-p=p14 onclick=nav(this)><span class=ic><svg viewBox=\"0 0 24"
		" 24\"><polygon points=\"8 5 19 12 8 19\"/></svg></span>试跑固件</button>\n"
#if IS_ENABLED(CONFIG_CMD_HTTPD_STOCK_RESTORE)
	"<button class=\"nav n4\" data-p=p4 onclick=nav(this)><span class=ic><svg viewBox=\"0 0 24"
		" 24\"><path d=\"M3 12a9 9 0 1 0 3-6.7M3 4v5h5\"/></svg></span>刷回原厂</button>\n"
#endif
	"<button class=\"nav n3\" data-p=p3 onclick=nav(this)><span class=ic><svg viewBox=\"0 0 24"
		" 24\"><path d=\"M4 7h16v10H4zM8 7v10M16 7v10\"/></svg></span>按卷写入</button>\n"
	"<div class=nsep></div>\n"
	"<button class=\"nav n9\" data-p=p10 onclick=nav(this)><span class=ic><svg viewBox=\"0 0 24"
		" 24\"><path d=\"M12 4v12M6 10l6 6 6-6M4 20h16\"/></svg></span>备份下载</button>\n"
	"<button class=\"nav n5\" data-p=p5 onclick=nav(this)><span class=ic><svg viewBox=\"0 0 24"
		" 24\"><rect x=\"4\" y=\"5\" width=\"16\" height=\"12\" rx=\"2\"/><path d=\"M8 21h8M12"
		" 17v4\"/></svg></span>设备详情</button>\n"
	"<button class=\"nav n7\" data-p=p8 onclick=nav(this)><span class=ic><svg viewBox=\"0 0 24"
		" 24\"><path d=\"M22 12h-4l-3 7-6-14-3 7H2\"/></svg></span>系统诊断</button>\n"
	"<button class=\"nav n10\" data-p=p11 onclick=nav(this)><span class=ic><svg viewBox=\"0 0 24"
		" 24\"><path d=\"M4 6h6M4 12h9M4 18h5M16 5l4 4-4 4\"/></svg></span>环境变量</button>\n"
	"<div class=nsep></div>\n"
	"<button class=\"nav n11\" data-p=p12 onclick=nav(this)><span class=ic><svg viewBox=\"0 0 24"
		" 24\"><path d=\"M12 3v9M18.4 6.6a9 9 0 1 1-12.8 0\"/></svg></span>启动与重启</button>\n"
	"<button class=\"nav n6\" data-p=p6 onclick=nav(this)><span class=ic><svg viewBox=\"0 0 24"
		" 24\"><circle cx=\"12\" cy=\"12\" r=\"9\"/><path d=\"M12 11v5M12 8h.01\"/></svg></span>关于</button>\n"
	"<div class=foot><span class=fbtns><button type=button class=tb onclick=tg() title=\"切换深浅色\""
		" aria-label=\"切换深浅色\"><svg viewBox=\"0 0 24 24\"><circle cx=\"12\" cy=\"12\" r=\"9\"/><path d=\"M12 3a9 9"
		" 0 0 1 0 18z\" fill=currentColor stroke=none/></svg></button><span class=lg data-raw"
		" role=radiogroup aria-label=\"Language\"><button type=button role=radio data-g=zh"
		" aria-checked=true lang=zh onclick=\"setlang('zh')\">中</button><button type=button role=radio"
		" data-g=en aria-checked=false lang=en onclick=\"setlang('en')\">EN</button></span></span><span"
		" class=live><span class=\"dot s1\" id=lived></span><span id=lives>连接中…</span></span></div>\n"
	"</div>\n"
	"<form method=post enctype=multipart/form-data onsubmit=\"return ask()\">\n"
	"<div class=ban id=ban hidden><svg viewBox=\"0 0 24 24\"><path d=\"M12 9v4M12 17h.01M10.3 3.9L2.4"
		" 18a2 2 0 0 0 1.7 3h15.8a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z\"/></svg><span"
		" id=bant></span></div>\n"
	"<div class=pane id=p1 data-on>\n"
	"<h1>日常刷机</h1><p class=sub>将 sysupgrade 固件写入 fit 卷。rootfs_data 将被清空；系统可正常启动时请使用 sysupgrade。</p>\n"
	"<label class=drop><input type=file name=firmware data-l=固件><span class=ic><svg viewBox=\"0 0 24"
		" 24\"><path d=\"M12 16V4M6 10l6-6 6 6M4 20h16\"/></svg></span><span class=tx><span class=t><span"
		" class=fn style=\"color:inherit\">选择固件文件</span></span><span class=\"s"
		" mono\">…-squashfs-sysupgrade.itb</span></span><span class=pb>选择文件…</span></label>\n"
	"<p class=note>设备停在恢复模式，不会自行引导。写入开始前闪存不会被修改；写完回读校验一遍，再由你决定是否重启。</p>\n"
	"<p class=note>想先不写闪存试一次，去「<a href=\"#\" onclick=\"return jump('p14')\">试跑固件</a>」。</p>\n"
	"<div class=act><span class=st>未选择文件</span><button type=submit class=\"pb"
		" pri\">上传并刷写</button></div>\n"
	"<div class=prog hidden><div class=pw><div class=pbar></div></div><div class=ps><span"
		" class=pwhat></span><span class=pct></span></div></div>\n"
	"</div>\n"
	"<div class=pane id=p2>\n"
	"<h1>引导升级</h1><p class=sub>写入 BL2 与 U-Boot FIP。用于从 tcboot / 原厂布局首次迁移，或升级 U-Boot。</p>\n"
	"<div class=box>\n"
	"<div class=fr><span class=fl>BL2<small class=mono>…-preloader.bin</small></span><label"
		" class=fc><input type=file name=bl2 data-l=BL2><span class=pb>选择文件…</span><span"
		" class=fn>未选择</span></label></div>\n"
	"<div class=fr><span class=fl>U-Boot<small class=mono>…-bl31-uboot.fip</small></span><label"
		" class=fc><input type=file name=fip data-l=U-Boot><span class=pb>选择文件…</span><span"
		" class=fn>未选择</span></label></div>\n"
	"<div class=fr><span class=fl>固件<small>可选，同时写入</small></span><label class=fc><input type=file"
		" name=firmware data-l=固件><span class=pb>选择文件…</span><span class=fn>未选择</span></label></div>\n"
	"</div>\n"
	"<p class=bh>首次迁移</p>\n"
	"<div class=box>\n"
	"<div class=\"fr wide\"><span class=fl>重建 UBI<small>擦除 ubi 分区并重新创建全部卷；出厂 MAC、U-Boot"
		" 环境与用户配置将丢失。<b>必须同时上传 BL2 与 U-Boot</b></small></span><span class=\"fc end\"><input type=checkbox"
		" class=sw name=format value=1></span></div>\n"
	"</div>\n"
	"<p class=note><b>重建 UBI 前先备份。</b>请先在「<a href=\"#\" onclick=\"return jump('p10')\">备份下载</a>」中导出。</p>\n"
	"<p class=note><b>重建擦除的范围从 0x20000 起，盖住了原厂引导器的后半截</b>（原厂 bootloader 分区是 0x0–0x80000，而本布局的 bl2 只占"
		" 0x0–0x20000）。所以只写 U-Boot、不写 BL2 的话，重启时原厂 BL2 会起来、却找不到它的下一级——只能拆串口救。因此这一项要求 BL2 与 U-Boot"
		" 一起传。</p>\n"
	"<p class=note>仅升级 U-Boot 时不需重建 UBI：只选择 U-Boot 文件，rootfs_data 保留。</p>\n"
	"<div class=act><span class=st>未选择文件</span><button type=submit class=\"pb"
		" pri\">上传并刷写</button></div>\n"
	"<div class=prog hidden><div class=pw><div class=pbar></div></div><div class=ps><span"
		" class=pwhat></span><span class=pct></span></div></div>\n"
	"</div>\n"
	"<div class=pane id=p14>\n"
	"<h1>试跑固件</h1><p class=sub>把恢复固件载入内存直接引导，<b>闪存一个字节都不写</b>。起不来断电即恢复原系统。</p>\n"
	"<label class=drop><input type=file name=firmware data-l=固件><span class=ic><svg viewBox=\"0 0 24"
		" 24\"><polygon points=\"8 5 19 12 8 19\"/></svg></span><span class=tx><span class=t><span class=fn"
		" style=\"color:inherit\">选择恢复固件</span></span><span class=\"s"
		" mono\">…-initramfs-recovery.itb</span></span><span class=pb>选择文件…</span></label>\n"
	"<input type=checkbox name=tryboot value=1 checked hidden>\n"
	"<p class=note><b>要用 initramfs 恢复固件。</b>它的根文件系统随镜像一起进内存，不依赖闪存，所以 UBI 是空的、fit"
		" 卷没了也照样跑得起来。<b>sysupgrade 固件不适合这里</b>：它的根要由 fitblk 从闪存的 fit 卷里读（设备树的 rootdisk"
		" 指着那个卷），试跑时内核是新的、根还是闪存里那份旧的，多半起不来。</p>\n"
	"<p class=note>引导成功后本页面不再可用；要回到恢复页，断电重来即可。闪存没写过一个字节，原系统还在。</p>\n"
	"<div class=act><span class=st>未选择文件</span><button type=submit class=\"pb pri\">启动它</button></div>\n"
	"<div class=prog hidden><div class=pw><div class=pbar></div></div><div class=ps><span"
		" class=pwhat></span><span class=pct></span></div></div>\n"
	"</div>\n"
#if IS_ENABLED(CONFIG_CMD_HTTPD_STOCK_RESTORE)
	"<div class=pane id=p4>\n"
	"<h1>刷回原厂</h1><p class=sub>将镜像写入 flash 指定偏移，边接收边写入、不在内存中暂存，因此没有单独的上传阶段。偏移为 0"
		" 时整片写入，当前引导程序与本页面将被覆盖；重新迁移至 OpenWrt 需经 USB-TTL 串口。</p>\n"
	"<div class=box>\n"
	"<div class=fr><span class=fl>镜像<small class=mono>all_flash.bin</small></span><label"
		" class=fc><input type=file name=stock data-l=镜像><span class=pb>选择文件…</span><span"
		" class=fn>未选择</span></label></div>\n"
	"<div class=fr><span class=fl>写入偏移<small>十六进制，按擦除块对齐</small></span><span class=fc><input"
		" type=text name=stockoff class=mono value=0x0 style=\"width:130px\" oninput=stwipe()><span"
		" class=fn>0 为整片，亦可写入单个分区</span></span></div>\n"
	"<div class=\"fr wide\"><span class=fl>擦净尾部<small>镜像之后<b>直至片尾</b>的擦除块一并擦空；不擦则保留原有内容</small></span><"
		"span class=\"fc end\"><input type=checkbox class=sw name=wipe value=1 checked"
		" onchange=\"this.dataset.t=1\"></span></div>\n"
	"</div>\n"
	"<p class=note>镜像须来自本机备份，可在「<a href=\"#\" onclick=\"return jump('p10')\">备份下载</a>」中导出。长度上限为 flash 容量"
		" <span id=upmax>—</span>。<b>页面不校验镜像内容、机型与偏移是否匹配</b>，写错仅能通过串口恢复。整片写入耗时显著长于固件写入；<b>写入一旦开始，中断将使闪存处于"
		"不一致状态，须重传至成功后方可重启</b>。写入期间指示灯<b>流水</b>，与其他页面一致；本页写入与接收同时进行，<b>网线与电源都不能断</b>，进度以本页进度条为准。</p>\n"
	"<div class=act><span class=st>未选择文件</span><button type=submit class=\"pb pri\">刷写</button></div>\n"
	"<div class=prog hidden><div class=pw><div class=pbar></div></div><div class=ps><span"
		" class=pwhat></span><span class=pct></span></div></div>\n"
	"</div>\n"
#endif
	"<div class=pane id=p3>\n"
	"<h1>写入 UBI 卷</h1><p class=sub>按卷名写入 UBI 卷，卷不存在时按文件长度创建。物理位置由 UBI 层管理，不涉及 bl2 分区。</p>\n"
	"<div id=fvh hidden><p class=bh>出厂数据<small>文件须来自本机备份，可同时写入</small></p><div class=box"
		" id=fv></div></div>\n"
	"<p class=bh>任意卷<small>卷名错误将覆盖对应卷的内容</small></p>\n"
	"<div class=box>\n"
	"<div class=fr><span class=fl>卷名</span><span class=fc><input type=text name=ubivol"
		" placeholder=\"如 fip、fit\" pattern=\"[A-Za-z0-9_.-]{1,63}\"><span class=fn>仅限字母、数字与 _ -"
		" .</span></span></div>\n"
	"<div class=fr><span class=fl>内容</span><label class=fc><input type=file name=ubifile"
		" data-l=卷内容><span class=pb>选择文件…</span><span class=fn>未选择</span></label></div>\n"
	"</div>\n"
	"<p class=note>写完由你决定是否重启，所以可以连着写好几个卷。</p>\n"
	"<div class=act><span class=st>未选择文件</span><button type=submit class=\"pb"
		" pri\">上传并刷写</button></div>\n"
	"<div class=prog hidden><div class=pw><div class=pbar></div></div><div class=ps><span"
		" class=pwhat></span><span class=pct></span></div></div>\n"
	"</div>\n"
	"<div class=pane id=p10>\n"
	"<h1>备份下载</h1><p class=sub>读取闪存内容并下载。读取与传输同步进行，不限长度；传输期间设备可能暂停响应。</p>\n"
	"<div class=seg data-seg=g10 role=tablist><button type=button role=tab data-s=s101"
		" aria-controls=s101 aria-selected=true onclick=seg(this)>UBI 卷</button><button type=button"
		" role=tab data-s=s102 aria-controls=s102 aria-selected=false"
		" onclick=seg(this)>原始区段</button></div>\n"
	"<div class=sp data-g=g10 id=s101 data-on>\n"
	"<div class=box><table class=kv id=dl><tr><td>正在读取…</td></tr></table></div>\n"
	"</div>\n"
	"<div class=sp data-g=g10 id=s102>\n"
	"<p class=note>绕过 UBI 按 flash 偏移读取，偏移与「刷回原厂」一致。</p>\n"
	"<div class=box>\n"
	"<div class=fr><span class=fl>起始偏移<small>十六进制</small></span><span class=fc><input type=text"
		" id=dumpoff class=mono value=0x0 style=\"width:130px\"><span class=fn>0 为片首</span></span></div>\n"
	"<div class=fr><span class=fl>长度<small>十六进制</small></span><span class=fc><input type=text"
		" id=dumplen class=mono placeholder=\"留空到片尾\" style=\"width:130px\"><span class=fn"
		" id=dumphint>留空读至片尾</span></span></div>\n"
	"</div>\n"
	"<p class=note>整片读取的是闪存本身，不经过分区表，原厂布局下的 romfile、config 一并包含。</p>\n"
	"<p class=note><b>文件偏移等于 flash 偏移</b>，坏块在文件中保留占位，格式与 <code>dd</code> 镜像一致：外部"
		" <code>all_flash.bin</code> 可直接写入，此处导出的镜像亦可用于编程器。写回时坏块跳过而不压缩，其后内容位置不变。</p>\n"
	"<div class=row><button type=button class=pb id=dumpallb onclick=dumpall()>整片下载</button><button"
		" type=button class=\"pb pri\" onclick=dumpraw()>下载区段</button></div>\n"
	"</div>\n"
	"<div id=dllh hidden><p class=bh>已完成<small>crc32 基于实际传出的字节计算，可与本地文件核对</small></p><div"
		" class=box><table class=kv id=dll></table></div></div>\n"
	"<div class=act><span class=st id=dlh>选择卷，或填写偏移与长度</span></div>\n"
	"<div class=prog id=dlprog hidden><div class=pw><div class=pbar></div></div><div class=ps><span"
		" class=pwhat></span><span class=pct></span></div></div>\n"
	"<p class=note id=dlwhy hidden>传输期间设备不应答：U-Boot 的 TCP"
		" 栈同时只有一条连接，而这条连接正被下载占着，所以本页面问不到「传了多少字节」。<b>字节进度请看浏览器自己的下载栏</b>。传完这里会给出 crc32，可与本地文件核对。</p>\n"
	"</div>\n"
	"<div class=pane id=p5>\n"
	"<h1>设备详情</h1><p class=sub>运行时从设备树、MTD 与 UBI 读取。</p>\n"
	"<div class=seg data-seg=g5 role=tablist><button type=button role=tab data-s=s51"
		" aria-controls=s51 aria-selected=true onclick=seg(this)>硬件</button><button type=button role=tab"
		" data-s=s52 aria-controls=s52 aria-selected=false onclick=seg(this)>网络</button><button"
		" type=button role=tab data-s=s53 aria-controls=s53 aria-selected=false onclick=seg(this)>UBI"
		" 卷</button></div>\n"
	"<div class=sp data-g=g5 id=s51 data-on>\n"
	"<div class=box><table class=kv id=dev><tr><td colspan=2 class=c2>正在读取…</td></tr></table></div>\n"
	"</div>\n"
	"<div class=sp data-g=g5 id=s52>\n"
	"<div class=box><table class=kv id=net><tr><td>正在读取…</td></tr></table></div>\n"
	"<p class=note id=portn hidden>端口号为交换机内部顺序，与外壳丝印不一定对应；无响应的端口不列出。</p>\n"
	"<p class=bh>地址<small>三种模式互斥，应用后需以新地址重新打开本页面</small></p>\n"
	"<div class=box>\n"
	"<div class=fr><span class=fl>模式</span><span class=fc><select id=amode"
		" onchange=amodesw()><option value=server>DHCP 服务器 · 本机发地址</option><option"
		" value=static>静态地址</option><option value=client>DHCP 客户端 · 向上级路由要</option></select></span></div>\n"
	"<div class=fr id=arow1><span class=fl id=iplab>路由器 IP<small>本机地址，末位固定为 1</small></span><span"
		" class=fc><input type=text id=nip class=mono style=\"width:160px\" oninput=ippv()><span class=fn"
		" id=ipfn></span></span></div>\n"
	"<div class=fr id=arow2><span class=fl>子网掩码</span><span class=fc><input type=text id=nmask"
		" class=mono value=255.255.255.0 style=\"width:160px\"></span></div>\n"
	"<div class=\"fr wide\" id=arow3><span class=fl>保存到闪存<small>不保存则只在本次开机有效</small></span><span"
		" class=\"fc end\"><input type=checkbox class=sw id=nsave value=1></span></div>\n"
	"</div>\n"
	"<p class=note id=nh></p>\n"
	"<div class=row><button type=button class=\"pb pri\" onclick=applyaddr()>应用</button></div>\n"
	"<p class=note id=nboot></p>\n"
	"<p class=note id=dhsum hidden></p>\n"
	"<div id=dgwbox hidden><p class=bh>DHCP 服务器</p>\n"
	"<div class=box><div class=\"fr wide\"><span class=fl>下发网关<small>关掉后路由器不下发网关</small></span><span"
		" class=\"fc end\"><input type=checkbox class=sw id=ndgw checked"
		" onchange=dgwset()></span></div></div>\n"
	"<p class=note id=dgwh></p></div>\n"
	"</div>\n"
	"<div class=sp data-g=g5 id=s53>\n"
	"<div class=vbar id=vbar hidden></div>\n"
	"<div class=vleg id=vleg hidden></div>\n"
	"<p class=note id=ubih></p>\n"
	"<div class=box><table class=kv id=ubi><tr><td>正在读取…</td></tr></table></div>\n"
	"</div>\n"
	"</div>\n"
	"<div class=pane id=p8>\n"
	"<h1>系统诊断</h1><p class=sub>读取设备状态：关键位置检查、全片扫描，以及本次上电的控制台输出。</p>\n"
	"<div class=seg data-seg=g8 role=tablist><button type=button role=tab data-s=s81"
		" aria-controls=s81 aria-selected=true onclick=seg(this)>快速检查</button><button type=button"
		" role=tab data-s=s82 aria-controls=s82 aria-selected=false"
		" onclick=seg(this)>全片扫描</button><button type=button role=tab data-s=s83 aria-controls=s83"
		" id=logtab aria-selected=false onclick=seg(this)>串口日志</button></div>\n"
	"<div class=sp data-g=g8 id=s81 data-on>\n"
	"<p class=note>检查 BL2、坏块、UBI 各卷与 U-Boot MAC。读取闪存期间设备暂停响应，耗时数秒。</p>\n"
	"<div class=box><table class=kv id=chk><tr><td colspan=2 class=empty><b>未执行检查</b>点「开始检查」读取"
		" BL2、坏块与各卷状态</td></tr></table></div>\n"
	"<div class=row><span class=st id=chkh></span><button type=button class=pb id=chkb"
		" onclick=check()>开始检查</button><button type=button class=pb"
		" onclick=diag(this)>复制诊断信息</button><button type=button class=pb"
		" onclick=diagfile(this)>下载诊断包</button></div>\n"
	"</div>\n"
	"<div class=sp data-g=g8 id=s82>\n"
	"<div class=box><table class=kv id=scan><tr><td class=empty><b>未扫描</b>点「开始扫描」逐页读一遍整片闪存</td></tr><"
		"/table></div>\n"
	"<p class=note>逐页读取全片；只读，不改动闪存。「快速检查」只覆盖关键位置，颗粒退化需整片读取才能发现。分段执行，段间页面保持响应；整片耗时数分钟，其间设备响应变慢。</p>\n"
	"<div class=prog id=scanprog hidden><div class=pw><div class=pbar></div></div><div"
		" class=ps><span class=pwhat></span><span class=pct></span></div></div>\n"
	"<div class=row><span class=st id=scanh></span><button type=button class=pb id=scanb"
		" onclick=scanrun()>开始扫描</button></div>\n"
	"</div>\n"
	"<div class=sp data-g=g8 id=s83>\n"
	"<p class=note>U-Boot 本次上电以来的控制台输出。</p>\n"
	"<div class=box><pre class=log id=log data-ph>未读取</pre></div>\n"
	"<p class=note id=logovf hidden>日志缓冲已满，后续输出未记录。重启后重新开始记录。</p>\n"
	"<div class=row><label><input type=checkbox class=sw id=logf"
		" onchange=logfollow()>实时跟随</label><button type=button class=pb id=logb"
		" onclick=getlog()>读取日志</button><button type=button class=pb"
		" onclick=\"copy(this,$('#log').textContent)\">复制日志</button></div>\n"
	"</div>\n"
	"</div>\n"
	"<div class=pane id=p11>\n"
	"<h1>U-Boot 环境变量</h1><p class=sub>当前生效的变量，只读。bootcmd 与引导菜单异常是无法启动的常见原因。</p>\n"
	"<div class=seg data-seg=g11 role=tablist><button type=button role=tab data-s=s111"
		" aria-controls=s111 aria-selected=true onclick=seg(this)>变量</button><button type=button"
		" role=tab data-s=s112 aria-controls=s112 aria-selected=false"
		" onclick=seg(this)>引导菜单预览</button><button type=button role=tab data-s=s113 aria-controls=s113"
		" aria-selected=false onclick=seg(this)>恢复默认</button></div>\n"
	"<div class=sp data-g=g11 id=s111 data-on>\n"
	"<div class=bar><input type=text id=envq placeholder=\"过滤名称或值\" oninput=envfill()"
		" style=\"max-width:200px\"><label><input type=checkbox class=sw id=envkey"
		" onchange=envfill()>只看关键项</label><span class=fs id=envh></span></div>\n"
	"<div class=box><table class=kv id=envt><tr><td>正在读取…</td></tr></table></div>\n"
	"<div class=row><button type=button class=pb id=envb onclick=getenv()>重新读取</button><button"
		" type=button class=pb onclick=envcopy(this)>复制全部</button></div>\n"
	"</div>\n"
	"<div class=sp data-g=g11 id=s112>\n"
	"<p class=note id=bmh>设备启动时的菜单，由 bootmenu_* 变量构成。</p>\n"
	"<div class=box><table class=kv id=bmenu><tr><td>正在读取…</td></tr></table></div>\n"
	"</div>\n"
	"<div class=sp data-g=g11 id=s113>\n"
	"<div class=box>\n"
	"<div class=\"fr wide\"><span class=fl>恢复为本版 U-Boot 的默认值<small>清除自定义的"
		" bootcmd、引导菜单与网络设置并保存</small></span><span class=\"fc end\"><span class=fs id=envdh></span><button"
		" type=button class=\"pb red\" onclick=askenvdef()>恢复默认</button></span></div>\n"
	"</div>\n"
	"<p class=note>出厂 MAC 位于 ri 卷，<b>不受影响</b>；固件与用户配置同样不受影响。恢复后需重启生效。UBI 无法挂载时保存失败，改动仅存于内存。</p>\n"
	"</div>\n"
	"</div>\n"
	"<div class=pane id=p12>\n"
	"<h1>启动与重启</h1><p class=sub>离开本页面的三种方式，均不改动闪存。</p>\n"
	"<p class=note>重启后正常引导，<b>引导成功就离开本页面了</b> —— 之后这个地址上是系统自己的页面。只有引导失败才回到恢复页，届时本页面自动刷新。</p>\n"
	"<div class=act><span class=st></span><button type=button class=\"pb red\""
		" onclick=askreboot()>立即重启</button></div>\n"
	"<p class=bh>其他启动方式</p>\n"
	"<div class=box>\n"
	"<div class=\"fr wide\"><span class=fl>直接启动系统<small>执行"
		" bootcmd，不经过冷启动；引导失败回到本页面</small></span><span class=\"fc end\"><button type=button class=pb"
		" onclick=askboot()>启动系统</button></span></div>\n"
	"<div class=\"fr wide\"><span class=fl>下次开机进恢复页<small>仅生效一次，进入后自动还原</small></span><span class=\"fc"
		" end\"><button type=button class=pb id=bob onclick=bootonce()>设置</button></span></div>\n"
	"</div>\n"
	"<p class=note id=boh>设置后下次开机停在本页面，再下次开机恢复正常引导。</p>\n"
	"<p class=bh>清空设置</p>\n"
	"<div class=box>\n"
	"<div class=\"fr wide\"><span class=fl>清空系统设置<small>删掉 OpenWrt"
		" 的设置和装过的软件包，固件与出厂数据不动</small></span><span class=\"fc end\"><button type=button class=\"pb red\""
		" id=wcb onclick=askwipe()>清空</button></span></div>\n"
	"</div>\n"
	"<p class=note id=wch></p>\n"
	"</div>\n"
	"<div class=pane id=p6>\n"
	"<h1>关于</h1><p class=sub>U-Boot 内置的 HTTP 恢复服务，页面不依赖任何外部资源。</p>\n"
	"<div class=about><span class=logo><svg viewBox=\"0 0 24 24\"><path d=\"M12 16V4M6 10l6-6 6 6M4"
		" 20h16\"/></svg></span><div><b>Airoha Web U-Boot</b><span>"
		WEB_VERSION
		" · <a href=\""
		AUTHOR_URL
		"\" target=_blank rel=noopener title=\""
		AUTHOR_HOST
		"\">"
		AUTHOR
		"</a></span></div></div>\n"
	"<div class=box style=\"margin-top:1rem\"><table class=kv>\n"
	"<tr><td>作者</td><td><a href=\""
		AUTHOR_URL
		"\" target=_blank rel=noopener title=\""
		AUTHOR_HOST
		"\">"
		AUTHOR
		"</a></td></tr>\n"
	"<tr><td>项目主页</td><td><a href=\""
		PROJECT_URL
		"\" target=_blank rel=noopener>"
		PROJECT_HOST
		"</a></td></tr>\n"
	"<tr><td>门户</td><td><a href=\""
		PORTAL_URL
		"\" target=_blank rel=noopener>"
		PORTAL_HOST
		"</a></td></tr>\n"
	"<tr><td>问题反馈</td><td><a href=\""
		PROJECT_URL
		"/issues\" target=_blank rel=noopener>Issues</a></td></tr>\n"
	"<tr><td>基于</td><td id=based>U-Boot</td></tr>\n"
	"</table></div>\n"
	"</div>\n"
	"<div class=pane id=p7>\n"
	"<h1>已交给设备启动</h1><p class=sub>设备正在从内存引导该固件，闪存未改动。引导成功后本页面不再可用。</p>\n"
	"<div class=fin><span class=ic><svg viewBox=\"0 0 24 24\"><path d=\"M5 12l5 5L20"
		" 7\"/></svg></span><div><b>起不来怎么办</b>\n"
	"<table class=lt><tr><td>断电</td><td>再上电即回到原有系统，闪存一个字节都没写</td></tr><tr><td>等待</td><td>引导失败时设备自行回到本"
		"页面，届时自动刷新</td></tr></table></div></div>\n"
	"<p class=bh>本次动作</p>\n"
	"<div class=box style=\"padding:.7rem 1rem\"><ol class=steps id=steps></ol></div>\n"
	"</div>\n"
#if IS_ENABLED(CONFIG_CMD_HTTPD_STOCK_RESTORE)
	"<div class=pane id=p13>\n"
	"<h1>写入完成</h1><p class=sub>镜像已全部写入闪存，设备正在重启。本页面可以关闭。</p>\n"
	"<div class=fin><span class=ic><svg viewBox=\"0 0 24 24\"><path d=\"M5 12l5 5L20"
		" 7\"/></svg></span><div><b>设备回报</b>\n"
	"<table class=lt id=stres></table></div></div>\n"
	"<p class=note>crc32 由设备对收到的字节计算，与备份时记录的值一致即说明传输无误。</p>\n"
	"<p class=note>重启约需 1–2 分钟。整片写入后本页面所属的 U-Boot 已被覆盖，设备将按镜像中的引导程序启动。</p>\n"
	"</div>\n"
#endif
	"</form>\n"
	"<iframe id=dlsink hidden title=download></iframe>\n"
	"<div class=mask id=off><div class=alert>\n"
	"<div class=hd><span class=\"mi bad\" id=offm><svg viewBox=\"0 0 24 24\" id=offi></svg></span><h2"
		" id=offt></h2></div>\n"
	"<div id=offb></div>\n"
	"<div class=btns><button type=button class=pb id=offr onclick=reconnect()>重新连接</button></div>\n"
	"</div></div>\n"
	"<div class=mask id=mask onclick=\"if(event.target===this)hide()\"><div class=alert>\n"
	"<div class=hd><span class=mi><svg viewBox=\"0 0 24 24\"><path d=\"M12 9v4M12 17h.01M10.3 3.9L2.4"
		" 18a2 2 0 0 0 1.7 3h15.8a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4"
		" 0z\"/></svg></span><h2>确认写入<small id=atitle></small></h2></div>\n"
	"<div id=abody></div>\n"
	"<div class=btns><button type=button class=pb onclick=hide()>取消</button><button type=button"
		" class=\"pb red\" id=yes onclick=go()>仍要写入</button></div>\n"
	"</div></div>\n"
	"<div class=mask id=rmask><div class=alert>\n"
	"<div class=hd><span class=\"mi ok\"><svg viewBox=\"0 0 24 24\"><path d=\"M5 12l5 5L20"
		" 7\"/></svg></span><h2 id=rtitle>写入完毕</h2></div>\n"
	"<div id=rbody></div>\n"
	"<div class=btns><button type=button class=pb onclick=rhide()>留在恢复页</button><button type=button"
		" class=\"pb red\" onclick=rboot()>立即重启</button></div>\n"
	"</div></div>\n"
	"</div>\n"
	"<script>\n"
	"var INFO=null,FV=[],SENT=null,CHK=null,ENV=null,YES=null,INFOQ=0,CHKQ=0,ENVQ=0,HB=0,HBT=null,HBG"
		"EN=0,HBOFF=0,LIVE=1,MISS=0,UP=-1,OFFWHY=\"\",SILENT=0,GONE=0,TRYB=0,RT=0,RN=0,RATE=0,DLSEQ=0,DLBAD"
		"=0,STUCK=\"\",STUCKN=null,DLWHAT=\"\",LOGN=0,LOGT=null,LOGGEN=0,NETINIT=0,NET=null,NETT=null,WRSPD=1"
		"4e5;try{var wr0=+localStorage.getItem(\"xgwrspd\");wr0>0&&(WRSPD=wr0)}catch(e){}var"
		" WR=null,WRT=null,WRP=null;function $(e,t){return(t||document).querySelector(e)}function"
		" $$(e,t){return Array.prototype.slice.call((t||document).querySelectorAll(e))}function tg(){var"
		" e=document.documentElement,t=\"dark\"==e.getAttribute(\"data-t\")?\"light\":\"dark\";e.setAttribute(\"da"
		"ta-t\",t);try{localStorage.setItem(\"xgtheme\",t)}catch(e){}}function sz(e){return"
		" e>=1048576?(e/1048576).toFixed(1)+\" MiB\":e>=1024?(e/1024).toFixed(0)+\" KiB\":e+\" B\"}function"
		" spd(e){return e>=1048576?(e/1048576).toFixed(1)+\" MiB/s\":e>=1024?(e/1024).toFixed(0)+\""
		" KiB/s\":Math.round(e)+\" B/s\"}function dur(e){e=Math.round(e);var t=\"en\"==LANG;if(e<60)return"
		" e+(t?\" s\":\" 秒\");var o=Math.floor(e/60);return o<60?o+(t?\" min \":\" 分 \")+e%60+(t?\" s\":\""
		" 秒\"):Math.floor(o/60)+(t?\" h \":\" 小时 \")+o%60+(t?\" min\":\" 分\")}function esc(e){return"
		" String(e).replace(/&/g,\"&amp;\").replace(/</g,\"&lt;\").replace(/\"/g,\"&quot;\").replace(/'/g,\"&#39;"
		"\")}function pane(){return $(\".pane[data-on]\")}function"
		" show(e){$$(\".pane\").forEach(function(t){t.id==e?t.setAttribute(\"data-on\",\"\"):t.removeAttribute("
		"\"data-on\")})}function seg(e){var t=e.parentNode,o=t.getAttribute(\"data-seg\"),n=e.getAttribute(\"d"
		"ata-s\");$$(\"button\",t).forEach(function(t){t.setAttribute(\"aria-selected\",t==e?\"true\":\"false\")})"
		",$$(\".sp[data-g=\"+o+\"]\").forEach(function(e){e.id==n?e.setAttribute(\"data-on\",\"\"):e.removeAttrib"
		"ute(\"data-on\")}),\"g8\"==o&&(\"s83\"==n?$(\"#logf\").checked||getlog():logstop()),\"g5\"==o&&netpoll(\"s5"
		"2\"==n)}function logstop(){var e=$(\"#logf\");e&&e.checked&&(e.checked=!1,logfollow())}function"
		" nav(e){$$(\".nav\").forEach(function(t){t===e?t.setAttribute(\"aria-current\",\"true\"):t.removeAttri"
		"bute(\"aria-current\")});var t=e.getAttribute(\"data-p\");show(t),\"p5\"!=t||INFO||INFOQ||info(),netpo"
		"ll(\"p5\"==t&&$(\"#s52\").hasAttribute(\"data-on\")),\"p8\"!=t&&logstop(),\"p8\"!=t||CHK||CHKQ||check(),\"p"
		"10\"==t&&dlfill(),\"p11\"!=t||ENV||ENVQ||getenv()}function jump(e){var"
		" t=$(\".nav[data-p=\"+e+\"]\");return t&&t.click(),!1}function busy(e){var"
		" t=$(\"#app\");e?t.setAttribute(\"data-busy\",\"\"):t.removeAttribute(\"data-busy\"),$$(\".nav\").forEach("
		"function(t){t.disabled=!!e})}function files(e){return"
		" $$(\"input[type=file]\",e).filter(function(e){return"
		" e.files&&e.files.length}).map(function(e){return{k:e.name,l:e.getAttribute(\"data-l\"),f:e.files["
		"0]}})}function onfile(e){var t=e.files[0],o=e.parentNode,n=$(\".fn\",o);if(t){n.textContent=t.name"
		",n.classList.add(\"has\"),o.classList.add(\"has\");var"
		" a=$(\".fs\",o);a||((a=document.createElement(\"span\")).className=\"fs\",n.parentNode.insertBefore(a,"
		"n.nextSibling)),a.textContent=sz(t.size),refresh()}}function refresh(){var"
		" e=pane(),t=$(\".st\",e);if(t){var o=files(e),n=0;o.forEach(function(e){n+=e.f.size}),t.textConten"
		"t=o.length?\"已选 \"+o.length+\" 个文件 · \"+sz(n):\"未选择文件\"}}function"
		" drag(e,t){e&&!e.__drag&&(e.__drag=1,e.ondragover=function(t){t.preventDefault(),e.classList.add"
		"(\"over\")},e.ondragleave=function(){e.classList.remove(\"over\")},e.ondrop=function(o){if(o.prevent"
		"Default(),e.classList.remove(\"over\"),o.dataTransfer&&o.dataTransfer.files.length)try{t.files=o.d"
		"ataTransfer.files,onfile(t)}catch(e){}})}function"
		" bind(){$$(\"input[type=file]\").forEach(function(e){e.onchange=function(){onfile(e)},drag(e.close"
		"st?e.closest(\".drop\")||e.closest(\".fr\"):null,e)})}function hex(e){return"
		" e=(e||\"\").trim(),/^(0x)?[0-9a-fA-F]{1,8}$/.test(e)?parseInt(e,16):-1}function stwipe(){var"
		" e=$(\"[name=wipe]\");e&&!e.dataset.t&&(e.checked=0===hex($(\"[name=stockoff]\").value))}function"
		" fvrows(){var e=\"\";FV.forEach(function(t){e+=\"<div class=fr><span"
		" class=fl>\"+esc(t.n)+(t.d?\"<small>\"+esc(t.d)+\"</small>\":\"\")+'</span><label class=fc><input"
		" type=file name=\"fvol_'+esc(t.n)+'\" data-l=\"'+esc(t.n)+'\"><span class=pb>选择文件…</span><span"
		" class=fn>未选择</span></label></div>'}),$(\"#fv\").innerHTML=e,$(\"#fvh\").hidden=!FV.length,bind()}fu"
		"nction get(e,t){var o=new XMLHttpRequest;o.onload=function(){t(o.status,o.responseText)},o.onerr"
		"or=function(){t(0,\"\")},o.open(\"GET\",e),o.send()}function"
		" info(){INFOQ?INFOQ=2:(INFOQ=1,get(\"/info\",function(e,t){var"
		" o=null,n=2==INFOQ;if(INFOQ=0,200==e)try{o=JSON.parse(t)}catch(e){}INFO=o,fill(),n&&info()}))}fu"
		"nction netget(){GONE||get(\"/net\",function(e,t){var"
		" o=null;try{o=JSON.parse(t)}catch(e){}o&&o.net&&(NET=o,INFO&&(INFO.net=o.net,INFO.ports=o.ports)"
		",netfill())})}function netpoll(e){NETT&&(clearInterval(NETT),NETT=null),e&&(netget(),NETT=setInt"
		"erval(netget,3e3))}function banner(){var e=\"\";STUCK?e=STUCK:INFO&&(INFO.ubi?INFO.ubi.fip||(e=\"<b"
		">闪存中没有 U-Boot（fip 卷）。</b>当前 U-Boot 仅存于内存，掉电丢失。请在「引导升级」中上传 U-Boot 文件。\"):e=\"<b>闪存中没有可挂载的"
		" UBI。</b>首次迁移：在「引导升级」中同时上传 BL2、U-Boot 与固件，并启用「重建"
		" UBI」。\"),$(\"#bant\").innerHTML=e,e?$(\"#ban\").removeAttribute(\"hidden\"):$(\"#ban\").setAttribute(\"hi"
		"dden\",\"\")}var ICON_OFF='<path d=\"M2 8.8a16 16 0 0 1 6-3.4M16 5.4a16 16 0 0 1 6 3.4M5 12.5a11 11"
		" 0 0 1 3.5-2.2M15.5 10.3a11 11 0 0 1 3.5 2.2M9 16.1a6 6 0 0 1 6 0M12 20h.01M2 2l20"
		" 20\"/>',ICON_RB='<path d=\"M12 3v9M18.4 6.6a9 9 0 1 1-12.8 0\"/>';function live(e){var"
		" t=$(\"#lived\"),o=$(\"#lives\");t&&(t.className=\"dot"
		" s\"+e,o.textContent=0==e?\"已连接\":SILENT?\"设备忙…\":1==e?\"无响应…\":\"已断开\")}function"
		" hbstart(){HB=1,ping()}function hbstop(){HB=0,HBGEN++,clearTimeout(HBT),HBT=null}function"
		" hbnext(){HB&&(clearTimeout(HBT),HBT=setTimeout(ping,LIVE?3e3:2e3))}function"
		" ping(){if(HB)if(HBOFF)hbnext();else{var e=HBGEN,t=new"
		" XMLHttpRequest,o=0,n=function(t,n){o||(o=1,e==HBGEN&&(seen(t,n),hbnext()))};t.timeout=2500,t.on"
		"load=function(){var e=-1,o=null;try{o=JSON.parse(t.responseText)}catch(e){}o&&(void"
		" 0!==o.up&&(e=o.up),$(\"#logovf\").hidden=!o.ovf),n(200==t.status&&e>=0,e)},t.onerror=function(){n"
		"(0,-1)},t.ontimeout=function(){n(0,-1)},t.open(\"GET\",\"/ping\"),t.send()}}function"
		" seen(e,t){if(e){if(SILENT=0,!LIVE){if(TRYB||\"reboot\"==OFFWHY||UP>=0&&t>=0&&t<UP)return void"
		" location.reload();LIVE=1,online()}return MISS=0,t>=0&&(UP=t),void"
		" live(0)}MISS++,LIVE&&MISS>=2&&(!SILENT||MISS>=40)&&(LIVE=0,SILENT=0,offline(OFFWHY)),live(LIVE?"
		"1:2)}function expect(e){OFFWHY=e,MISS=0,LIVE=0,offline(e),live(2)}function"
		" silent(){SILENT=1,MISS=0}function offline(e){if(!GONE&&!TRYB){var"
		" t,o,n,a=0;\"reboot\"==e?(t=ICON_RB,o=\"设备正在重启\",n=\"引导成功即进入系统，<b>本页面不再可用</b>；引导失败才回到这里并自动刷新。\"):\"boot"
		"\"==e?(t=ICON_RB,o=\"设备正在启动系统\",n=\"闪存未改动。引导成功后<b>本页面不再可用</b>；引导失败则回到本页面。\"):(t=ICON_OFF,o=\"与设备的连接已断开"
		"\",n=\"设备无响应，请检查网线与电源。指示灯仍在流水说明它还活着，请稍候。\",a=1),$(\"#offi\").innerHTML=t,$(\"#offm\").className=\"mi"
		" \"+(a?\"bad\":\"info\"),$(\"#offt\").textContent=o,$(\"#offb\").innerHTML='<div class=w"
		" style=\"color:inherit\">'+n+\"</div>\"+(a?\"<div class=hint>每 2"
		" 秒自动重试。</div>\":\"\"),$(\"#offr\").hidden=!a,$(\"#offr\").disabled=!1,$(\"#offr\").textContent=\"重新连接\",$(\""
		"#off\").setAttribute(\"data-on\",\"\")}}function online(){OFFWHY=\"\",$(\"#off\").removeAttribute(\"data-o"
		"n\")}function reconnect(){var e=$(\"#offr\");e.disabled=!0,e.textContent=\"正在重试…\",clearTimeout(HBT),"
		"HB||(HB=1),ping()}function askreboot(){var e=\"<div"
		" class=w>闪存内容不受影响</div>\";INFO&&!INFO.ubi?e+=\"<div class=w><b>闪存上没有可挂载的 UBI，当前 U-Boot"
		" 仅存于内存。</b>重启后回到原有系统，需重新经串口传入 U-Boot 才能再打开本页面。建议先在「引导升级」中完成写入</div>\":INFO&&INFO.ubi&&!INFO.ubi.f"
		"ip&&(e+=\"<div class=w><b>闪存中没有 U-Boot（fip 卷），当前 U-Boot"
		" 仅存于内存。</b>重启后本页面将无法再打开</div>\"),$(\"#atitle\").textContent=\"重启\",$(\"#abody\").innerHTML=\"<div"
		" class=r><span>动作</span><span class=v>reset</span></div>\"+e;var"
		" t=$(\"#yes\");t.hidden=!1,t.textContent=\"立即重启\",YES=doreboot,$(\"#mask\").setAttribute(\"data-on\",\"\")"
		"}function doreboot(){get(\"/reboot\",function(){}),expect(\"reboot\")}var MODES={server:\"DHCP"
		" 服务器\",static:\"静态地址\",client:\"DHCP 客户端\"};function amodesw(){var"
		" e=$(\"#amode\").value;$(\"#dgwbox\").hidden=\"server\"!=e,$(\"#arow1\").hidden=\"client\"==e,$(\"#arow2\")."
		"hidden=\"static\"!=e,$(\"#iplab\").innerHTML=\"server\"==e?\"路由器 IP<small>本机地址，末位固定为"
		" 1</small>\":\"IP<small>设备的接口地址</small>\",$(\"#nh\").innerHTML=\"server\"==e?\"电脑直接插到本设备上时，插上就能拿到地址，不必手动"
		"配 IP。掩码固定 <b>255.255.255.0</b>，电脑拿到的是本网段 <b>.100</b>，不带"
		" DNS。<b>接入已有网络前不要用这一档</b>：它对任何请求都应答，会和该网络的路由器抢着发地址，被抢到的机器会断网。\":\"static\"==e?\"接入已有网络时用这一档：填一个该网段内的"
		"空闲地址，本机不再发地址，应用后即可从网内任何一台机器打开本页面。\":\"地址由上级路由分配，本页面事先不知道是多少，需在上级路由的客户端列表中按 MAC"
		" 查找。本机不再发地址。未取得租约时退回当前地址。\",ippv(),bootline()}function ippv(){var"
		" e,t=$(\"#amode\").value,o=$(\"#nip\").value.trim(),n=$(\"#ipfn\");\"client\"!=t?\"static\"!=t?IP4.test(o)"
		"?(e=o.replace(/\\.\\d+$/,\"\"),n.innerHTML=\"设备 <b>\"+esc(e)+\".1</b> · 电脑"
		" <b>\"+esc(e)+\".100</b>\"):n.textContent=\"填本网段内任一地址即可，如"
		" 192.168.1.1\":n.textContent=\"主机需在同一网段\":n.textContent=\"\"}function bootdesc(){var"
		" e=INFO&&INFO.net&&INFO.net.saved;return e?MODES[e.mode]+(\"client\"==e.mode?\"\":\""
		" \"+e.ip+(\"static\"==e.mode?\" / \"+e.mask:\"\")):\"DHCP 服务器 192.168.1.1 /"
		" 255.255.255.0（出厂默认）\"}function bootline(){$(\"#nboot\").innerHTML=\"<b>下次开机：\"+esc(bootdesc())+\"</b>"
		"。勾上「保存到闪存」才会把当前这一套写进去替换它；不勾就只管本次开机。\"}var IP4=/^(\\d{1,3}\\.){3}\\d{1,3}$/;function applyaddr(){var"
		" e,t=$(\"#amode\").value,o=$(\"#nip\").value.trim(),n=\"server\"==t?\"255.255.255.0\":$(\"#nmask\").value."
		"trim()||\"255.255.255.0\",a=$(\"#nsave\").checked;if(\"client\"!=t){if(!IP4.test(o))return"
		" void($(\"#nh\").textContent=\"IP 不是一个合法的地址\");if(\"server\"==t)o=o.replace(/\\.\\d+$/,\".1\");else"
		" if(!IP4.test(n))return void($(\"#nh\").textContent=\"子网掩码不是一个合法的地址\")}$(\"#atitle\").textContent=\"改网络"
		"模式\",e=\"<div class=r><span>模式</span><span class=v>\"+MODES[t]+\"</span></div>\"+(\"client\"==t?\"\":\"<di"
		"v class=r><span>地址</span><span class=v>\"+esc(o)+(\"static\"==t?\" / \"+esc(n):\" /"
		" 255.255.255.0\")+\"</span></div>\"),\"server\"==t&&(e+=\"<div class=r><span>网关</span><span"
		" class=v>\"+($(\"#ndgw\").checked?\"下发\":\"不下发\")+\"</span></div><div class=w>本机开始发地址：电脑将拿到"
		" <b>\"+esc(o.replace(/\\.\\d+$/,\".100\"))+\"</b></div>\"),\"static\"==t&&(e+=\"<div"
		" class=w>本机<b>不再发地址</b>，电脑需手动配置同网段的 IP</div>\"),\"client\"==t&&(e+=\"<div"
		" class=w><b>地址由上级路由分配，本页面无法预知。</b>请在上级路由的客户端列表中按 MAC 查找：\"+esc(INFO&&INFO.mac||\"\")+\"</div><div"
		" class=w>本机<b>不再发地址</b>；未取得租约时退回当前地址</div>\"),\"client\"!=t&&(e+=\"<div class=w>本页面将断开，需以"
		" http://\"+esc(o)+\"/ 重新打开</div>\"),e+=a?\"<div class=w><b>保存到闪存</b>：下次开机就用这一套。地址填错时断电也无法恢复</div>\":\""
		"<div class=w>不保存：只管本次开机，下次开机回到 <b>\"+esc(bootdesc())+\"</b></div>\",INFO&&INFO.net&&INFO.net.ack&&\""
		"server\"!=t&&(e+=\"<div class=w><b>本机用的是设备发的地址</b>：本机这个地址马上就没人续租了，\"+(\"static\"==t?\"需手动把本机配成"
		" \"+esc(o.replace(/\\.\\d+$/,\".x\"))+\" 的一个地址\":\"请改用上级路由那个网络里的机器\")+\"</div>\"),$(\"#abody\").innerHTML=e;v"
		"ar i=$(\"#yes\");i.hidden=!1,i.textContent=\"应用\",YES=function(){donetmode(t,o,n,a)},$(\"#mask\").setA"
		"ttribute(\"data-on\",\"\")}function donetmode(e,t,o,n){get(\"/netmode?mode=\"+e+(\"client\"==e?\"\":\"&ip=\""
		"+encodeURIComponent(t)+\"&mask=\"+encodeURIComponent(o))+\"&save=\"+(n?1:0),function(o,a){200==o&&/^"
		"ok /.test((a||\"\").trim())?(hbstop(),\"client\"==e?gone(\"正在获取地址\",\"获取成功后设备位于新地址，请在上级路由的客户端列表中按"
		" <b>\"+esc(INFO&&INFO.mac||\"\")+\"</b> 查找。未取得租约时退回原地址。\"+(n?\"\":\"下次开机回到"
		" <b>\"+esc(bootdesc())+\"</b>。\"),\"\"):gone(\"设备已移至 \"+t,\"本页面连接的是旧地址，不会自动恢复。请以"
		" <b>http://\"+esc(t)+\"/</b> 重新打开。\"+(n?\"\":\"下次开机回到 <b>\"+esc(bootdesc())+\"</b>。\"),\"http://\"+t+\"/\")):"
		"$(\"#nh\").textContent=\"设备没有接受：\"+((a||\"\").trim()||o)})}function"
		" gone(e,t,o){GONE=1,netpoll(0),$(\"#offi\").innerHTML=ICON_OFF,$(\"#offm\").className=\"mi"
		" info\",$(\"#offt\").textContent=e,$(\"#offb\").innerHTML='<div class=w"
		" style=\"color:inherit\">'+t+\"</div>\"+(o?\"<div class=hint>10"
		" 秒后自动跳转。</div>\":\"\"),$(\"#offr\").hidden=!0,$(\"#off\").setAttribute(\"data-on\",\"\"),o&&setTimeout(func"
		"tion(){location.href=o},1e4)}function dgwset(){var"
		" e=$(\"#ndgw\"),t=e.checked?1:0;e.disabled=!0,get(\"/dhcpgw?on=\"+t,function(o,n){if(e.disabled=!1,n"
		"=(n||\"\").trim(),200!=o||!/^ok/.test(n))return e.checked=!t,void($(\"#dgwh\").textContent=503==o?\"设"
		"备正在写入，稍后再试\":\"设备没有接受：\"+(n||o));var a=NET&&NET.net||INFO&&INFO.net;a&&(a.dgw=t),$(\"#dgwh\").textCon"
		"tent=(t?\"已打开\":\"已关闭\")+(n.indexOf(\"saved\")<0?\"，但未能保存到闪存\":\"\")+\"。电脑重新插拔网线后生效\"})}function"
		" wcvol(){var e=null;return(INFO&&INFO.ubi&&INFO.ubi.vols||[]).forEach(function(t){\"rootfs_data\"="
		"=t.n&&(e=t)}),e}function wcfill(){var e=$(\"#wcb\"),t=$(\"#wch\");INFO&&\"已清空\"!=e.textContent&&(e.dis"
		"abled=!INFO.ubi||!wcvol(),t.textContent=INFO.ubi?wcvol()?\"\":\"没有 rootfs_data"
		" 卷，下次启动就是全新系统\":\"闪存上没有 UBI，没有可清空的设置\")}function askwipe(){var"
		" e,t=bkall();$(\"#atitle\").textContent=\"清空系统设置\",e=\"<div class=r><span>删除</span><span"
		" class=v>rootfs_data 卷</span></div><div class=w>OpenWrt"
		" 的设置和软件包全部清空，下次启动为全新系统</div>\",t.rootfs_data||t.all||(e='<div class=w>rootfs_data 未备份 · <a"
		" href=\"#\" onclick=\"gobk();return false\">去备份</a></div>'+e),$(\"#abody\").innerHTML=e;var"
		" o=$(\"#yes\");o.hidden=!1,o.textContent=\"清空\",YES=dowipe,$(\"#mask\").setAttribute(\"data-on\",\"\")}fun"
		"ction dowipe(){var e=$(\"#wcb\");e.disabled=!0,$(\"#wch\").textContent=\"正在清空…\",get(\"/wipecfg\",functi"
		"on(t,o){if(o=(o||\"\").trim(),200!=t||!/^ok/.test(o))return"
		" e.disabled=!1,void($(\"#wch\").textContent=503==t?\"设备正在写入，稍后再试\":\"设备没有接受：\"+(o||t));CHK=null,e.text"
		"Content=\"已清空\",$(\"#wch\").textContent=\"已清空。下次启动为全新系统\",info()})}function"
		" askboot(){$(\"#atitle\").textContent=\"启动系统\",$(\"#abody\").innerHTML=\"<div"
		" class=r><span>动作</span><span class=v>run bootcmd</span></div><div"
		" class=w>闪存内容不受影响；引导失败回到本页面</div>\";var e=$(\"#yes\");e.hidden=!1,e.textContent=\"启动系统\",YES=doboot,$"
		"(\"#mask\").setAttribute(\"data-on\",\"\")}function doboot(){get(\"/boot\",function(){}),expect(\"boot\")}"
		"function bootonce(){var e=$(\"#bob\");e.disabled=!0,get(\"/bootonce\",function(t,o){e.disabled=!1,CH"
		"K=null,ENV=null,o=(o||\"\").trim(),200==t?(e.textContent=o.indexOf(\"armed\")<0?\"未设置\":o.indexOf(\"sav"
		"ing failed\")>=0?\"未保存\":\"已设置\",$(\"#boh\").textContent=o.indexOf(\"armed\")<0?\"设备没有接受：\"+o:o.indexOf(\"sa"
		"ving failed\")>=0?\"已设置，但未能保存到闪存：断电后失效，请在「诊断」中确认 ubootenv"
		" 卷\":o.indexOf(\"already\")>=0?\"此前已设置，下次开机将停在本页面\":\"已设置。下次开机停在本页面，再下次开机恢复正常引导\"):$(\"#boh\").textConten"
		"t=\"设置失败（\"+t+\"）\"})}function dlfill(){var e=$(\"#dl\"),t=INFO&&INFO.flash,o={},n=\"\";t&&($(\"#dumphint"
		"\").textContent=\"读至片尾，\"+sz(t.size)),INFO?INFO.ubi?((INFO.fv||[]).forEach(function(e){o[e.n]=e.s})"
		",(INFO.ubi.vols||[]).slice().sort(function(e,t){return(o[t.n]?1:0)-(o[e.n]?1:0)||(e.n<t.n?-1:e.n"
		">t.n?1:0)}).forEach(function(e){var t=e.u||e.s;o[e.n]&&t>o[e.n]&&(t=o[e.n]),n+=\"<tr><td>\"+esc(e."
		"n)+(o[e.n]?\"<span class=tag>出厂数据</span>\":\"\")+\"</td><td>\"+esc(e.t)+\"</td><td"
		" class=n>\"+sz(t)+'</td><td class=b><button type=button class=\"pb dlb\""
		" data-v=\"'+esc(e.n)+'\">下载</button></td></tr>'}),e.innerHTML=n,dlbind(e)):e.innerHTML=\"<tr><td"
		" class=empty><b>UBI 未挂载</b>首次迁移前闪存仍为原厂内容，建议此时用「原始区段」整片备份</td></tr>\":e.innerHTML=INFOQ?\"<tr><td>正"
		"在读取…</td></tr>\":\"<tr><td class=empty>读取失败，刷新页面重试</td></tr>\"}function"
		" dlbind(e){e.__b||(e.__b=1,e.addEventListener(\"click\",function(t){for(var"
		" o=t.target;o&&o!=e;){if(o.getAttribute&&/(^| )dlb( |$)/.test(o.className||\"\"))return void"
		" dlvol(o.getAttribute(\"data-v\"));o=o.parentNode}}))}function dlstart(e,t){var"
		" o=$(\"#dlsink\");o.onload=function(){dlrefused(o)},o.src=e}function dlrefused(e){var"
		" t=\"\";try{t=e.contentDocument&&e.contentDocument.body?e.contentDocument.body.textContent:\"\"}catc"
		"h(e){}(t=t.replace(/\\s+/g,\" \").trim())&&(DLBAD=1,dlstop(\"bad\"),$(\".pwhat\",$(\"#dlprog\")).textCont"
		"ent=\"设备拒绝了本次备份\",$(\"#dlh\").textContent=\"设备拒绝了本次备份：\"+t)}function sink(e,t,o){silent(),DLBAD=0;var"
		" n=$(\"#dlprog\");n.hidden=!1,n.className=\"prog\",$(\".pbar\",n).className=\"pbar"
		" ind\",$(\".pbar\",n).style.width=\"100%\",$(\".pwhat\",n).textContent=\"正在读取并传送"
		" \"+t,$(\".pct\",n).textContent=null===o?\"到片尾\":sz(o),$(\"#dlwhy\").hidden=!1,$(\"#dlh\").textContent=\"正"
		"在读取并传送 \"+t+\"（\"+(null===o?\"到片尾\":sz(o))+\"）…\",get(\"/dumpinfo\",function(t,n){var"
		" a=null;try{a=JSON.parse(n)}catch(e){}DLSEQ=a&&a.seq||0,dlstart(e,o),dlpoll(0)})}function"
		" dlpoll(e){DLBAD||get(\"/dumpinfo\",function(t,o){if(!DLBAD){var"
		" n=null;try{n=JSON.parse(o)}catch(e){}if(200==t&&n&&n.seq&&n.seq!=DLSEQ)return DLSEQ=n.seq,void"
		" dlrow(n);e<120?setTimeout(function(){dlpoll(e+1)},2e3):(dlstop(\"bad\"),$(\"#dlh\").textContent=\"未收"
		"到设备回报，本次备份可能未完成，请查看「诊断」中的串口日志\")}})}function dlstop(e){var"
		" t=$(\"#dlprog\");t.className=\"prog\"+(e?\" \"+e:\"\"),$(\"#dlwhy\").hidden=!0,$(\".pbar\",t).className=\"pb"
		"ar\",$(\".pbar\",t).style.width=\"100%\",$(\".pct\",t).textContent=\"\"}function bkall(){var"
		" e=INFO&&INFO.mac,t={};if(!e)return{};try{t=JSON.parse(localStorage.getItem(\"xgbk\")||\"{}\")||{}}c"
		"atch(e){}return t[e]||{}}function bkmark(e){var t=INFO&&INFO.mac,o={};if(t&&e)try{((o=JSON.parse"
		"(localStorage.getItem(\"xgbk\")||\"{}\")||{})[t]=o[t]||{})[e]=Date.now(),localStorage.setItem(\"xgbk\""
		",JSON.stringify(o))}catch(e){}}function bkmissing(){var e=bkall();return"
		" e.all?[]:FV.map(function(e){return e.n}).filter(function(t){return!e[t]})}function"
		" gobk(){hide(),nav($(\".nav[data-p=p10]\")),INFO&&INFO.ubi||(seg($(\"[data-s=s102]\")),$(\"#dumpallb\""
		").focus())}function dlrow(e){var t=$(\"#dll\"),o=0|e.holes;o||bkmark(DLWHAT),t.innerHTML+=\"<tr><td"
		">\"+esc(e.name)+(o?\"<span class=tag>\"+o+\" 块读取失败</span>\":\"\")+\"</td><td"
		" class=n>\"+sz(e.len)+'</td><td class=\"n mono\">'+esc(e.crc)+\"</td></tr>\",$(\"#dllh\").hidden=!1,dls"
		"top(o?\"\":\"ok\"),$(\".pwhat\",$(\"#dlprog\")).textContent=o?o+\" 个块读取失败，该部分以 0xff"
		" 填充\":\"传输完成\",$(\"#dlh\").textContent=o?\"传输完成；\"+o+\" 个块读取失败，该部分以 0xff"
		" 填充，详见「诊断」中的串口日志\":\"传输完成\"}function dlvol(e){var t,o=null;DLWHAT=e,(INFO&&INFO.ubi&&INFO.ubi.vols|"
		"|[]).forEach(function(t){t.n==e&&(o=t)}),t=o?o.u||o.s:0,FV.forEach(function(o){o.n==e&&(!t||t>o."
		"s)&&(t=o.s)}),sink(\"/dump?vol=\"+encodeURIComponent(e),e+\" 卷\",t)}function"
		" dumpall(){INFO&&INFO.flash?($(\"#dumpoff\").value=\"0x0\",$(\"#dumplen\").value=\"\",dumpraw()):$(\"#dlh"
		"\").textContent=\"无法读取闪存信息\"}function dumpraw(){var"
		" e=INFO&&INFO.flash,t=hex($(\"#dumpoff\").value),o=$(\"#dumplen\").value.trim(),n=o?hex(o):null;t<0?"
		"$(\"#dlh\").textContent=\"起始偏移不是有效的十六进制数\":null!==n&&n<0?$(\"#dlh\").textContent=\"长度不是有效的十六进制数\":0!==n?"
		"e&&t>=e.size?$(\"#dlh\").textContent=\"起始偏移超出 flash 容量"
		" \"+sz(e.size):e&&null!==n&&t+n>e.size?$(\"#dlh\").textContent=\"偏移加长度超过 flash 容量"
		" \"+sz(e.size):(DLWHAT=t||null!==n?\"\":\"all\",sink(\"/dump?off=0x\"+t.toString(16)+(null===n?\"\":\"&len"
		"=0x\"+n.toString(16)),\"0x\"+t.toString(16)+\" 起的区段\",n)):$(\"#dlh\").textContent=\"长度为 0\"}var"
		" ENVKEY=/^(bootcmd|bootdelay|bootmenu_|ethaddr|ipaddr|serverip|netmask|loadaddr|boot_|check_butt"
		"ons|web_uboot_|envver|httpd_|ubi_)/;function getenv(){var"
		" e=$(\"#envb\");e.disabled=!0,ENVQ=1,get(\"/env\",function(t,o){ENVQ=0,e.disabled=!1;var"
		" n=null;try{n=JSON.parse(o)}catch(e){}503!=t?n&&n.env?(ENV=n,envfill(),bmfill()):$(\"#envt\").inne"
		"rHTML=\"<tr><td class=empty>读取失败，刷新页面重试</td></tr>\":$(\"#envt\").innerHTML=\"<tr><td"
		" class=empty>设备正在写入，稍后再试</td></tr>\"})}function envrows(){if(!ENV)return[];var"
		" e=$(\"#envq\").value.trim().toLowerCase(),t=$(\"#envkey\").checked;return"
		" ENV.env.filter(function(o){return!(t&&!ENVKEY.test(o.k))&&(!e||(o.k.toLowerCase().indexOf(e)>=0"
		"||String(o.v).toLowerCase().indexOf(e)>=0))})}function envfill(){var"
		" e=$(\"#envt\"),t=envrows(),o=\"\";ENV&&(t.forEach(function(e){o+=\"<tr><td>\"+esc(e.k)+\"</td><td"
		" class=envv>\"+esc(e.v)+\"</td></tr>\"}),e.innerHTML=o||\"<tr><td"
		" class=empty>没有匹配的变量</td></tr>\",$(\"#envh\").textContent=t.length+\" / \"+ENV.env.length+\""
		" 项\"+(ENV.cut?\" · 已截断，完整内容见串口\":\"\"))}function bmkey(e){return"
		" e<9?String(e+1):e<35?String.fromCharCode(88+e):\"?\"}function bmfill(){var"
		" e,t=$(\"#bmenu\"),o={},n=\"\";ENV?(ENV.env.forEach(function(t){var"
		" n=/^bootmenu_(\\d+)$/.exec(t.k);n&&(o[+n[1]]=t.v),\"bootmenu_delay\"==t.k&&(e=t.v)}),Object.keys(o"
		").map(Number).sort(function(e,t){return e-t}).forEach(function(e){var"
		" t=o[e],a=t.indexOf(\"=\"),i=a<0?t:t.slice(0,a),s=a<0?\"\":t.slice(a+1),r=/\\x1b\\[[0-9;]*3[147]m/.tes"
		"t(i);i=i.replace(/\\x1b\\[[0-9;?]*[A-Za-z]/g,\"\"),n+=\"<tr><td class=n>\"+bmkey(e)+\".</td><td\"+(r?\""
		" class=hot\":\"\")+\">\"+esc(i)+\"</td><td class=cmd>\"+esc(s)+\"</td></tr>\"}),t.innerHTML=n||\"<tr><td"
		" class=empty>没有 bootmenu_* 条目</td></tr>\",$(\"#bmh\").textContent=n?\"设备启动时的菜单，序号即串口上按的键\"+(e?\"；\"+e+\""
		" 秒内无按键则执行 bootcmd\":\"\")+\"。红色条目会写入闪存\":\"环境中没有引导菜单：串口不会停顿，直接执行"
		" bootcmd\"):t.innerHTML=\"<tr><td>正在读取…</td></tr>\"}function"
		" envcopy(e){copy(e,envrows().map(function(e){return e.k+\"=\"+e.v}).join(\"\\n\"))}function"
		" askenvdef(){$(\"#atitle\").textContent=\"环境变量\",$(\"#abody\").innerHTML=\"<div"
		" class=r><span>动作</span><span class=v>env default -a && saveenv</span></div><div class=w>自定义的"
		" bootcmd、引导菜单与网络设置将恢复为本版 U-Boot 的默认值</div><div class=w>出厂 MAC、固件与用户配置不受影响；恢复后需重启生效</div>\";var"
		" e=$(\"#yes\");e.hidden=!1,e.textContent=\"恢复默认\",YES=envdef,$(\"#mask\").setAttribute(\"data-on\",\"\")}f"
		"unction envdef(){$(\"#envdh\").textContent=\"正在恢复…\",get(\"/envreset\",function(e,t){200==e?(CHK=null,"
		"$(\"#envdh\").textContent=t&&t.indexOf(\"saved\")>=0?\"已恢复并保存，重启后生效\":\"已恢复，但保存失败（UBI"
		" 未挂载），改动仅存于内存\",getenv()):$(\"#envdh\").textContent=\"恢复失败（\"+e+\"）\"})}function netfill(){var"
		" e,t=$(\"#net\"),o=NET&&NET.net||INFO&&INFO.net,n=NET&&NET.ports||INFO&&INFO.ports||[],a=\"\";o?(e=o"
		".mode||\"server\",a+=\"<tr><td>模式</td><td>\"+(MODES[e]||e)+(o.ram?\"<span"
		" class=tag>未保存</span>\":\"\")+\"</td></tr>\",a+=\"<tr><td>地址</td><td"
		" class=mono>\"+esc(o.ip)+(o.mask&&\"0.0.0.0\"!=o.mask?\" /"
		" \"+esc(o.mask):\"\")+\"</td></tr>\",o.gw&&\"0.0.0.0\"!=o.gw&&(a+=\"<tr><td>网关</td><td"
		" class=mono>\"+esc(o.gw)+\"</td></tr>\"),a+=\"<tr><td>网卡</td><td"
		" class=mono>\"+esc(o.dev||\"\")+\"</td></tr>\",$(\"#dhsum\").hidden=\"server\"!=e,$(\"#ndgw\").disabled||($"
		"(\"#ndgw\").checked=0!==o.dgw),$(\"#dhsum\").textContent=o.ack?\"已发出 \"+o.ack+\""
		" 个地址\"+(o.client?\"，最近一次分配给 \"+o.client:\"\")+\"。当前打开本页面的主机用的就是设备发的地址。\":o.offer?\"已发出 \"+o.offer+\""
		" 次地址，均未被接受；当前主机用的是手动配置的 IP。\":\"未发出过地址；当前主机用的是手动配置的 IP，更换主机后需重新配置。\",n.forEach(function(e){var"
		" t=0|e.p,o=0|e.speed,n=0|e.fd;a+='<tr><td><span class=\"dot s'+(e.link?0:1)+'\"></span>端口"
		" '+t+\"</td><td\"+(e.link?\"\":\" class=s1\")+\">\"+(e.link?(o?o>=1e3?o/1e3+\" Gb/s\":o+\""
		" Mb/s\":\"已连接\")+(o?n?\" 全双工\":\" 半双工\":\"\"):\"未连接\")+\"</td></tr>\"}),t.innerHTML=a,$(\"#portn\").hidden=!n.l"
		"ength,NETINIT?bootline():(NETINIT=1,$(\"#amode\").value=e,$(\"#nip\").value||($(\"#nip\").value=o.ip||"
		"\"\"),o.mask&&\"0.0.0.0\"!=o.mask&&($(\"#nmask\").value=o.mask),amodesw())):t.innerHTML=\"<tr><td"
		" class=empty>读不到网络信息</td></tr>\"}var VCOL={fip:\"#5e5ce6\",fit:\"#0a84ff\",rootfs_data:\"#30d158\",uboo"
		"tenv:\"#8e8e93\",ubootenv2:\"#8e8e93\",ri:\"#ff9f0a\",bosa:\"#af52de\"},VPOOL=[\"#00b4a0\",\"#a2845e\",\"#ff4"
		"53a\",\"#64748b\"],VRECL={fit:1,rootfs_data:1};function ubibar(){var"
		" e,t,o,n=$(\"#vbar\"),a=$(\"#vleg\"),i=INFO&&INFO.ubi,s=0,r=0,l=\"\",d=\"\";if(!i||!i.pebs||!i.leb)retur"
		"n n.hidden=!0,a.hidden=!0,void($(\"#ubih\").textContent=\"\");e=i.leb*i.pebs,(i.vols||[]).slice().so"
		"rt(function(e,t){return(0|e.i)-(0|t.i)}).forEach(function(e,t){var"
		" o=VCOL[e.n]||VPOOL[t%VPOOL.length],n=VRECL[e.n]?\""
		" re\":\"\",a=0|e.s;s+=a,VRECL[e.n]&&(r+=a),l+='<i class=\"'+n+'\" style=\"flex:'+a+\" 0"
		" 0;background:\"+o+'\" title=\"'+esc(e.n)+\" \"+sz(a)+'\"></i>',d+='<span><em class=\"'+n+'\""
		" style=\"background:'+o+'\"></em>'+esc(e.n)+\" \"+sz(a)+(VRECL[e.n]?\" ·"
		" 写入时腾出\":\"\")+\"</span>\"}),(o=e-s)<0&&(o=0),null==(t=null==i.avail?null:(0|i.avail)*i.leb)||t>o?(l+"
		"='<i class=free style=\"flex:'+o+' 0 0\" title=\"空闲与预留 '+sz(o)+'\"></i>',d+=\"<span><em"
		" class=free></em>空闲与 UBI 预留 \"+sz(o)+\"</span>\"):(r+=t,l+='<i class=free style=\"flex:'+t+' 0 0\""
		" title=\"空闲 '+sz(t)+'\"></i><i class=rsv style=\"flex:'+(o-t)+' 0 0\" title=\"UBI 预留"
		" '+sz(o-t)+'\"></i>',d+=\"<span><em class=free></em>空闲 \"+sz(t)+'</span><span><em"
		" style=\"background:var(--c3)\"></em>UBI 预留 '+sz(o-t)+\"</span>\"),n.innerHTML=l,a.innerHTML=d,n.hid"
		"den=!1,a.hidden=!1,$(\"#ubih\").textContent=\"逻辑擦除块 \"+sz(i.leb)+\" × \"+i.pebs+\"。宽度为各卷的预留容量，按卷 ID"
		" 排列；UBI 卷在闪存中并不连续，此图不表示物理位置\"+(null==t?\"\":\"。刷机可用 \"+Math.floor(r/1048576)+\" MiB：当前空闲"
		" \"+sz(t)+\"，加上写入时会腾出的 fit 与 rootfs_data\")}function fill(){var"
		" e=$(\"#dev\"),t=$(\"#ubi\"),o=\"\";return banner(),netfill(),INFO?([[\"机型\",INFO.model],[\"SoC\",INFO.soc"
		"],[\"内存\",INFO.ram?sz(INFO.ram):\"\"],[\"闪存\",INFO.flash?INFO.flash.name+\" \"+sz(INFO.flash.size)+\" ·"
		" 擦除块 \"+sz(INFO.flash.erase)+\" · 页 \"+sz(INFO.flash.page):\"\"],[\"分区\",(INFO.parts||[]).map(function("
		"e){return e.n+\" 0x\"+e.o.toString(16)+\"–0x\"+(e.o+e.s).toString(16)}).join(\" ·"
		" \")],[\"MAC\",INFO.mac],[\"U-Boot\",INFO.uboot]].forEach(function(e){e[1]&&(o+=\"<tr><td>\"+e[0]+\"</td"
		"><td class=mono>\"+esc(e[1])+\"</td></tr>\")}),e.innerHTML=o,INFO.uboot&&($(\"#based\").textContent=I"
		"NFO.uboot),$(\"#logtab\").hidden=!INFO.log,INFO.flash&&$$(\"#upmax\").forEach(function(e){e.textCont"
		"ent=sz(INFO.flash.size)}),dlfill(),FV=INFO.fv||[],fvrows(),wcfill(),INFO.ubi?(ubibar(),o=\"<tr><t"
		"h class=n>ID</th><th>名称</th><th>类型</th><th class=n>大小</th><th"
		" class=n>已用</th></tr>\",(INFO.ubi.vols||[]).slice().sort(function(e,t){return"
		" e.n<t.n?-1:e.n>t.n?1:0}).forEach(function(e){o+=\"<tr><td"
		" class=n>\"+(null==e.i?\"\":0|e.i)+\"</td><td>\"+esc(e.n)+\"</td><td>\"+esc(e.t)+\"</td><td"
		" class=n>\"+sz(e.s)+\"</td><td class=n>\"+sz(e.u)+\"</td></tr>\"}),void(t.innerHTML=o)):(t.innerHTML="
		"\"<tr><td class=empty>UBI 未挂载</td></tr>\",void ubibar())):(e.innerHTML=\"<tr><td colspan=2"
		" class=empty>读取失败，刷新页面重试</td></tr>\",void(t.innerHTML=\"\"))}function check(){var"
		" e=$(\"#chkb\"),t=$(\"#chk\");CHKQ=1,silent(),e.disabled=!0,e.textContent=\"检查中…\",t.innerHTML=\"<tr><t"
		"d colspan=2 class=empty>正在检查，读取闪存期间设备不响应…</td></tr>\",get(\"/check\",function(o,n){CHKQ=0,e.disable"
		"d=!1,e.textContent=\"重新检查\";var a=null;try{a=JSON.parse(n)}catch(e){}if(503!=o)if(a&&a.items){CHK="
		"a.items;var i=\"\",s=[0,0,0],r=\"\";a.items.forEach(function(e){var"
		" t=Math.max(0,Math.min(2,0|e.s));s[t]++,e.g&&e.g!=r&&(r=e.g,i+=\"<tr><th class=g"
		" colspan=2>\"+esc(r)+\"</th></tr>\"),i+='<tr><td><span class=\"dot"
		" s'+t+'\"></span>'+esc(e.n)+'</td><td class=\"s'+t+'\">'+esc(e.v)+\"</td></tr>\"}),t.innerHTML=i,$(\"#"
		"chkh\").textContent=s[2]?s[2]+\" 项异常 · \"+s[1]+\" 项注意 · \"+s[0]+\" 项正常\":s[1]?s[1]+\" 项注意 · \"+s[0]+\""
		" 项正常\":\"全部 \"+s[0]+\" 项正常\"}else t.innerHTML=\"<tr><td colspan=2"
		" class=empty>读取失败，刷新页面重试</td></tr>\";else t.innerHTML=\"<tr><td colspan=2"
		" class=empty>设备正在写入，稍后再试</td></tr>\"})}function logfollow(){clearTimeout(LOGT),LOGT=null,LOGGEN++"
		";var e=$(\"#logf\").checked;$(\"#logb\").disabled=e,e&&(LOGN=0,logset(\"\",1),logtick())}function"
		" logtick(){if($(\"#logf\").checked){var e=LOGGEN;get(\"/log?from=\"+LOGN,function(t,o){if(e==LOGGEN&"
		"&$(\"#logf\").checked){var n=200==t&&o?o.indexOf(\"\\n\"):-1;if(n>0){var"
		" a=parseInt(o.slice(0,n),10);if(a>=LOGN){LOGN=a;var"
		" i=o.slice(n+1).replace(/\\x1b\\[[0-9;?]*[A-Za-z]/g,\"\").replace(/\\r/g,\"\");if(i){var"
		" s=$(\"#log\");s.textContent+=i,s.scrollTop=s.scrollHeight}}}LOGT=setTimeout(logtick,2e3)}})}}var"
		" SCAN=null;function scanrun(){var e=$(\"#scanb\"),t=$(\"#scanprog\");if(SCAN)return"
		" SCAN.stop=1,SCAN=null,e.textContent=\"重新扫描\",t.className=\"prog"
		" bad\",$(\".pwhat\",t).textContent=\"已停止\",void($(\"#scanh\").textContent=\"已停止，结果仅覆盖已扫描的部分\");SCAN={off:"
		"0,size:0,blk:0,bad:0,ecc:0,fail:0,badl:[],faill:[],t0:Date.now(),rt:Date.now(),rn:0,rate:0,stop:"
		"0},e.textContent=\"停止\",t.hidden=!1,t.className=\"prog\",$(\".pbar\",t).className=\"pbar\",$(\".pbar\",t)."
		"style.width=\"0\",$(\".pwhat\",t).textContent=\"正在扫描\",$(\".pct\",t).textContent=\"\",$(\"#scanh\").textCont"
		"ent=\"\",scanstep()}function scanstep(){var e=SCAN;e&&!e.stop&&(silent(),get(\"/scan?off=0x\"+e.off."
		"toString(16),function(t,o){if(SCAN===e&&!e.stop){var"
		" n=null;try{n=JSON.parse(o)}catch(e){}if(200!=t||!n||void"
		" 0!==n.err){SCAN=null,$(\"#scanb\").textContent=\"重新扫描\";var a=$(\"#scanprog\");return"
		" a.className=\"prog bad\",void($(\".pwhat\",a).textContent=n&&n.err||\"扫描没能进行（\"+t+\"）\")}if(e.size=n.si"
		"ze,e.blk=n.blk,e.off=n.off,e.bad+=n.bad,e.ecc+=n.ecc,e.fail+=n.fail,(n.badlist||[]).forEach(func"
		"tion(t){e.badl.length<32&&e.badl.push(t)}),(n.faillist||[]).forEach(function(t){e.faill.length<3"
		"2&&e.faill.push(t)}),scanshow(e,n.done),n.done)return"
		" SCAN=null,void($(\"#scanb\").textContent=\"重新扫描\");scanstep()}}))}function"
		" hx(e){return\"0x\"+e.toString(16)}function scanshow(e,t){var"
		" o=$(\"#scanprog\"),n=e.size?e.off/e.size:0,a=Date.now(),i=(a-e.rt)/1e3,s=\"\";i>=1.5&&(e.rate=(e.of"
		"f-e.rn)/i,e.rt=a,e.rn=e.off),$(\".pbar\",o).style.width=100*n+\"%\",$(\".pct\",o).textContent=sz(e.off"
		")+\" / \"+sz(e.size)+\" · \"+(100*n).toFixed(0)+\"%\"+(!t&&e.rate>0?\" · \"+spd(e.rate)+\" · 剩余"
		" \"+dur((e.size-e.off)/e.rate):\"\"),t&&(o.className=\"prog\"+(e.fail?\" bad\":\""
		" ok\"),$(\".pwhat\",o).textContent=e.fail?\"扫描完成，存在无法读出的页\":\"扫描完成\"),s+=\"<tr><td>已扫描</td><td>\"+sz(e.of"
		"f)+(e.size?\" / \"+sz(e.size):\"\")+\"（擦除块 \"+sz(e.blk||0)+\"）</td></tr>\",s+='<tr><td><span class=\"dot"
		" s'+(e.bad?1:0)+'\"></span>坏块</td><td class=s'+(e.bad?1:0)+\">\"+(e.bad?e.bad+\""
		" 个：\"+e.badl.map(hx).join(\"、\")+(e.bad>e.badl.length?\" …\":\"\"):\"无\")+\"</td></tr>\",s+='<tr><td><span"
		" class=\"dot s'+(e.ecc?1:0)+'\"></span>ECC 纠错</td><td class=s'+(e.ecc?1:0)+\">\"+(e.ecc?e.ecc+\""
		" 页在读取时被纠正。少量属正常；成片出现表明颗粒退化，应尽快备份\":\"无\")+\"</td></tr>\",s+='<tr><td><span class=\"dot"
		" s'+(e.fail?2:0)+'\"></span>读失败</td><td class=s'+(e.fail?2:0)+\">\"+(e.fail?e.fail+\""
		" 页：\"+e.faill.map(hx).join(\"、\")+(e.fail>e.faill.length?\" …\":\"\")+\"。ECC"
		" 无法纠正，这些位置的数据已丢失\":\"无\")+\"</td></tr>\",$(\"#scan\").innerHTML=s,$(\"#scanh\").textContent=t?e.fail?\"扫描完"
		"成，\"+e.fail+\" 页无法读出\":e.bad||e.ecc?\"扫描完成，见上表\":\"扫描完成，全片可读\":\"\"}function logset(e,t){var"
		" o=$(\"#log\");return t?o.setAttribute(\"data-raw\",\"\"):o.removeAttribute(\"data-raw\"),o.textContent="
		"e,o}function getlog(){var e=$(\"#logb\"),t=$(\"#log\");e.disabled=!0,t.hasAttribute(\"data-ph\")&&(t.r"
		"emoveAttribute(\"data-ph\"),logset(\"正在读取…\",0)),get(\"/log\",function(o,n){e.disabled=$(\"#logf\").chec"
		"ked,e.textContent=\"重新读取\",200==o?logset(n.replace(/\\x1b\\[[0-9;?]*[A-Za-z]/g,\"\").replace(/\\r/g,\"\")"
		",1):logset(\"读取失败（\"+o+\"）\",0),t.scrollTop=t.scrollHeight})}function copy(e,t){var o=!1;try{var"
		" n=document.createElement(\"textarea\");n.value=t,n.style.position=\"fixed\",n.style.opacity=\"0\",doc"
		"ument.body.appendChild(n),n.select(),o=document.execCommand(\"copy\"),document.body.removeChild(n)"
		"}catch(e){}!o&&navigator.clipboard&&navigator.clipboard.writeText(t);var"
		" a=e.textContent;e.textContent=\"已复制\",setTimeout(function(){e.textContent=a},1500)}function"
		" diag(e){var t=[\"Airoha Web U-Boot \"+(INFO&&INFO.web||\"\")];INFO&&($$(\"#dev"
		" tr\").forEach(function(e){t.push(e.cells[0].textContent+\":"
		" \"+e.cells[1].textContent)}),INFO.ubi?(INFO.ubi.vols||[]).forEach(function(e){t.push(\"vol"
		" \"+e.n+\" \"+e.t+\" \"+e.s+\" used \"+e.u)}):t.push(\"UBI:"
		" none\")),CHK?CHK.forEach(function(e){t.push([\"OK  \",\"WARN\",\"FAIL\"][e.s]+\" \"+e.n+\":"
		" \"+e.v)}):t.push(T(\"(未运行健康检查)\")),copy(e,t.join(\"\\n\"))}function diagtext(){var e=[\"Airoha Web"
		" U-Boot \"+(INFO&&INFO.web||\"\")];return e.push(T(\"导出于 \")+(new"
		" Date).toLocaleString()),e.push(\"\"),e.push(T(\"== 设备 ==\")),INFO?($$(\"#dev"
		" tr\").forEach(function(t){e.push(t.cells[0].textContent+\":"
		" \"+t.cells[1].textContent)}),INFO.ubi?(INFO.ubi.vols||[]).forEach(function(t){e.push(\"vol"
		" \"+t.n+\" \"+t.t+\" \"+t.s+\" used \"+t.u)}):e.push(\"UBI: none\")):e.push(T(\"(/info"
		" 读取失败)\")),e.push(\"\"),e.push(T(\"== 网络 ==\")),e.push($(\"#net\").textContent.replace(/\\s+/g,\""
		" \").trim()||T(\"(未读取)\")),e.push(\"\"),e.push(T(\"== 快速检查"
		" ==\")),CHK?CHK.forEach(function(t){e.push([\"OK  \",\"WARN\",\"FAIL\"][t.s]+\" \"+t.n+\":"
		" \"+t.v)}):e.push(T(\"(未运行)\")),e.push(\"\"),e.push(T(\"== 环境变量"
		" ==\")),ENV?ENV.env.forEach(function(t){e.push(t.k+\"=\"+t.v)}):e.push(T(\"(未读取)\")),e.push(\"\"),e.pus"
		"h(T(\"== 全片扫描 ==\")),e.push($(\"#scan .empty\")?T(\"(未扫描)\"):$(\"#scan\").textContent.replace(/\\s+/g,\""
		" \").trim()),e.push(\"\"),e.push(T(\"== 串口日志 ==\")),e.push($(\"#log\").textContent),e.join(\"\\n\")}functi"
		"on diagfile(e){var t=e.textContent;e.disabled=!0,e.textContent=\"正在收集…\";var"
		" o=function(){e.disabled=!1,e.textContent=t,save((INFO&&INFO.model||\"device\").replace(/[^A-Za-z0"
		"-9_.-]+/g,\"-\").toLowerCase()+\"-diag.txt\",diagtext())},n=function(){if(ENV)return"
		" o();get(\"/env\",function(e,t){var n=null;try{n=JSON.parse(t)}catch(e){}n&&n.env&&(ENV=n,envfill("
		"),bmfill()),o()})};if($(\"#log\").textContent.length>20)return"
		" n();get(\"/log\",function(e,t){200==e&&logset(t.replace(/\\x1b\\[[0-9;?]*[A-Za-z]/g,\"\").replace(/\\r"
		"/g,\"\"),1),n()})}function save(e,t){try{var o=new"
		" Blob([t],{type:\"text/plain;charset=utf-8\"}),n=URL.createObjectURL(o),a=document.createElement(\""
		"a\");a.href=n,a.download=e,document.body.appendChild(a),a.click(),document.body.removeChild(a),se"
		"tTimeout(function(){URL.revokeObjectURL(n)},1e3)}catch(e){}}function"
		" plan(){return[\"将固件载入内存并直接引导\",\"<b>不写入闪存</b>；引导失败断电即恢复原系统\"]}function ask(){var"
		" e=pane(),t=files(e),o=[],n=[],a=\"\",i=$(\"#yes\");if(\"p5\"==e.id)return"
		" applyaddr(),!1;if(\"p10\"==e.id||\"p11\"==e.id||\"p12\"==e.id)return!1;YES=null,i.textContent=\"仍要写入\";"
		"var s=$(\"[name=tryboot]\",e);if(s&&s.checked&&(t.some(function(e){return\"firmware\"==e.k})?t.lengt"
		"h>1?n.push(\"试跑仅接收固件本身，其他文件不会被写入\"):(o.push(\"固件仅载入内存，不写入闪存；引导失败断电即恢复原系统\"),/recovery/i.test(t[0].f."
		"name)||o.push(\"这个文件名不像 initramfs 恢复固件。sysupgrade 固件的根文件系统在闪存的 fit"
		" 卷里，试运行不写闪存也就用不到它，内核多半起不来\")):n.push(\"试跑需要选择一个恢复固件\"),i.textContent=\"启动它\"),$(\"#atitle\").textConten"
		"t=$(\"h1\",e).textContent,\"p4\"==e.id&&t.length){var"
		" r=hex($(\"[name=stockoff]\",e).value),l=INFO&&INFO.flash;if(r<0)n.push(\"写入偏移不是有效的十六进制数\");else{l&&"
		"r%l.erase&&n.push(\"写入偏移未按擦除块 \"+sz(l.erase)+\" 对齐\"),l&&r+t[0].f.size>l.size&&n.push(\"偏移加镜像长度超过"
		" flash 容量 \"+sz(l.size)),o.push(\"自 flash 偏移 0x\"+(r<0?\"?\":r.toString(16))+\" 起写入"
		" \"+sz(t[0].f.size)+(r?\"\":\"，覆盖 bootloader 及其后全部内容\")),o.push(\"写入随上传同步进行，中断将使闪存处于不一致状态，须重传至成功后方可重启\""
		");var d=$(\"[name=wipe]\",e),c=l?l.size-r-t[0].f.size:0;l&&c>0&&o.push(d&&d.checked?\"镜像之后剩余的"
		" \"+sz(c)+\" 将被擦成空白\":\"镜像之后剩余的 \"+sz(c)+\" 保留原有内容不动\"),r||o.push(\"写入后本页面不再可用，重新迁移需经串口\")}}if(\"p3\"==e.id"
		"){var f=$(\"[name=ubivol]\",e).value.trim(),h=t.some(function(e){return\"ubifile\"==e.k});h&&!f&&n.p"
		"ush(\"选择了卷内容但未填写卷名\"),f&&!h&&n.push(\"填写了卷名但未选择卷内容\"),f&&!/^[A-Za-z0-9_.-]{1,63}$/.test(f)&&n.push(\""
		"卷名仅限字母、数字与 _ - .\"),h&&f&&o.push(\"卷 \"+esc(f)+\" 不存在时按文件长度创建\")}if(\"p2\"==e.id){var"
		" u=$(\"[name=format]\",e).checked,p=t.some(function(e){return\"fip\"==e.k}),b=t.some(function(e){ret"
		"urn\"bl2\"==e.k});if(u&&!p&&n.push(\"打开了「重建 UBI」却没有选择 U-Boot 文件：重建会抹掉 fip 卷，没有 U-Boot"
		" 设备将无法启动\"),u&&!b&&n.push(\"打开了「重建 UBI」却没有选择 BL2：重建从 0x20000 起擦，盖住了原厂引导器的后半截，只写 U-Boot"
		" 的话重启起不来，只能拆串口救\"),INFO&&INFO.ubi&&!INFO.ubi.fip&&!p&&n.push(\"闪存里没有 U-Boot（fip 卷），本次必须同时上传"
		" U-Boot 文件\"),!INFO||INFO.ubi||u||n.push(\"闪存里没有可挂载的 UBI：请打开「重建 UBI」，并同时上传 BL2、U-Boot"
		" 与固件\"),u&&o.push(\"重建 UBI 将清除出厂 MAC、U-Boot 环境与用户配置\"),u)if(INFO&&INFO.ubi){var"
		" m=bkmissing();m.length&&o.unshift(esc(m.join(\"、\"))+' 未备份 · <a href=\"#\" onclick=\"gobk();return"
		" false\">去备份</a>')}else bkall().all||o.unshift('原厂系统请先整片备份 · <a href=\"#\" onclick=\"gobk();return"
		" false\">整片下载</a>');t.some(function(e){return\"bl2\"==e.k||\"fip\"==e.k})||o.push(\"未选择 BL2 或"
		" U-Boot，本次只写入固件\")}var v=null,g=null,w=$(\"[name=format]\",e)&&$(\"[name=format]\",e).checked;(INFO&&"
		"INFO.parts||[]).forEach(function(e){\"bl2\"==e.n&&(v=e)}),(INFO&&INFO.ubi&&INFO.ubi.vols||[]).forE"
		"ach(function(e){\"fip\"==e.n&&(g=e)});var I=g&&!w?g.s:1048576;t.forEach(function(e){\"bl2\"==e.k&&v&"
		"&e.f.size>v.s-2048&&n.push(\"BL2 文件 \"+sz(e.f.size)+\"，超过 bl2 分区自 0x800 起可写的"
		" \"+sz(v.s-2048)),\"fip\"==e.k&&I&&e.f.size>I&&n.push(\"U-Boot 文件 \"+sz(e.f.size)+\"，超过 fip 卷的"
		" \"+sz(I))}),\"p1\"!=e.id&&\"p3\"!=e.id||!INFO||INFO.ubi||n.push(\"闪存里没有可挂载的"
		" UBI，请先在「引导升级」里完成首次迁移\"),\"p1\"!=e.id&&\"p3\"!=e.id||!INFO||!INFO.ubi||INFO.ubi.fip||o.push(\"闪存里没有"
		" U-Boot（fip 卷）：写完不会自动重启，但重启之前要先到「引导升级」里补上 U-Boot"
		" 文件\"),(\"p1\"==e.id||\"p2\"==e.id&&t.some(function(e){return\"firmware\"==e.k}))&&o.push(\"rootfs_data"
		" 将被清空\");var k=0;t.forEach(function(e){k+=e.f.size}),\"p4\"!=e.id&&INFO&&INFO.uploadmax&&k+4096>INF"
		"O.uploadmax&&n.push(\"本次上传 \"+sz(k)+\"，超过设备单次可接收的 \"+sz(INFO.uploadmax)+\"：内存不足，设备将拒绝\");var"
		" y=INFO&&INFO.model;return y&&t.some(function(e){return MDK[e.k]})&&(a+=\"<div"
		" class=r><span>本机</span><span class=v>\"+esc(y)+\"</span></div>\"),t.forEach(function(e){var"
		" t=fcheck(e.k,e.f.name,y);a+=\"<div class=r><span>\"+esc(e.l)+\"</span>\"+(t?'<span class=vc><span"
		" class=\"v'+(t.c?\" \"+t.c:\"\")+'\">':\"<span class=v>\")+esc(e.f.name)+\" ·"
		" \"+sz(e.f.size)+\"</span>\"+(t?'<span class=\"md'+(t.c?\""
		" \"+t.c:\"\")+'\">'+t.t+\"</span></span>\":\"\")+\"</div>\"}),t.length||(a=\"<div"
		" class=r><span>未选择任何文件</span></div>\"),n.forEach(function(e){a+=\"<div"
		" class=e>\"+e+\"</div>\"}),o.forEach(function(e){a+=\"<div"
		" class=w>\"+e+\"</div>\"}),$(\"#abody\").innerHTML=a,i.hidden=!t.length||n.length>0,$(\"#mask\").setAtt"
		"ribute(\"data-on\",\"\"),!1}var EXT={firmware:\".itb\",bl2:\".bin\",fip:\".fip\",stock:\".bin\"},MDK={firmwa"
		"re:1,bl2:1,fip:1};function fmodel(e){var t=/an75\\d\\d-[a-z0-9]+_([a-z0-9-]+?)-(?:ubi-|squashfs|in"
		"itramfs|preloader|bl31)/i.exec(e);return t?t[1].toUpperCase():\"\"}function mnorm(e){return"
		" String(e).toLowerCase().replace(/[^a-z0-9]/g,\"\")}function fcheck(e,t,o){var n,a=EXT[e];return"
		" a?t.toLowerCase().slice(-a.length)!=a?{c:\"bad\",t:\"应为 \"+a+\""
		" 文件\"}:MDK[e]?(n=fmodel(t))?o?mnorm(o).indexOf(mnorm(n))>=0?{c:\"ok\",t:esc(n)+\" ·"
		" 与本机一致\"}:{c:\"bad\",t:esc(n)+\" · 本机是 \"+esc(String(o).replace(/^\\S+\\s+/,\"\"))}:{c:\"\",t:esc(n)}:{c:\"b"
		"ad\",t:\"文件名里没有机型\"}:null:null}function hide(){$(\"#mask\").removeAttribute(\"data-on\"),YES=null}funct"
		"ion go(){var e=YES;hide(),e?e():send()}function send(){var e,t=pane(),o=files(t),n=new"
		" FormData,a=new XMLHttpRequest,i=$(\".prog\",t),s=$(\".pbar\",i),r=$(\"button[type=submit]\",t),l=0;if"
		"($$(\"input[type=text]\",t).forEach(function(e){e.value.trim()&&n.append(e.name,e.value.trim())}),"
		"$$(\"input[type=checkbox]\",t).forEach(function(e){e.checked&&n.append(e.name,\"1\")}),o.forEach(fun"
		"ction(e){n.append(e.k,e.f,e.f.name),l+=e.f.size}),SENT={p:t,rows:o,btxt:r.textContent},WR={n:0,o"
		"ff:0,k:0,txt:\"\",rows:[],ver:null,secs:0,left:0,rt:Date.now(),rn:0,rate:0,fin:0},clearInterval(WR"
		"T),WRT=setInterval(wrclock,1e3),RT=Date.now(),RN=0,RATE=0,HBOFF=1,busy(1),r.disabled=!0,r.textCo"
		"ntent=\"p4\"==t.id?\"写入中…\":\"上传中…\",i.hidden=!1,i.className=\"prog\",s.className=\"pbar\",s.style.width=\""
		"0\",a.upload.onprogress=function(n){if(n.lengthComputable){var"
		" a=Math.min(n.loaded,l),r=0,d=o[0],c=0;for(e=0;e<o.length&&(d=o[e],c=e,!(a<r+o[e].f.size||e==o.l"
		"ength-1));e++)r+=o[e].f.size;s.style.width=a/l*100+\"%\",$(\".pwhat\",i).textContent=(\"p4\"==t.id?\"正在"
		"写入 \":\"正在上传 \")+d.l+(o.length>1?\"（\"+(c+1)+\"/\"+o.length+\"）\":\"\")+\" · \"+sz(Math.min(a-r,d.f.size))+\""
		" / \"+sz(d.f.size);var f=Date.now(),h=(f-RT)/1e3;h>=1.5&&(RATE=(a-RN)/h,RT=f,RN=a),$(\".pct\",i).te"
		"xtContent=sz(a)+\" / \"+sz(l)+\" · \"+(a/l*100).toFixed(0)+\"%\"+(RATE>0?\" · \"+spd(RATE)+\" · 剩余"
		" \"+dur((l-a)/RATE):\"\")}},a.upload.onload=function(){s.className=\"pbar ind\";var"
		" e=$(\"[name=wipe]\",t),o=$(\"[name=tryboot]\",t),n=!(!o||!o.checked);$(\".pwhat\",i).textContent=\"p4\""
		"==t.id?e&&e.checked?\"传输完成，设备正在写入最后一块并擦净尾部…\":\"传输完成，设备正在写入最后一块…\":n?\"已载入内存，即将启动\":\"上传完成，设备开始写入闪存\",$("
		"\".pct\",i).textContent=\"p4\"==t.id||n?\"\":\"预计 \"+dur(Math.max(8,l/WRSPD))},a.onload=function(){var"
		" e=a.responseText||\"\";200==a.status&&(\"p4\"==t.id?/^ok"
		" /.test(e):'{\"ok\":1}'==e.trim())?done(e):200!=a.status?fail((a.status>=500?\"设备写入失败（\":\"设备拒绝了上传（\")"
		"+a.status+\"）：\"+a.responseText,a.status):fail(\"设备正忙：另一个上传还没结束，请稍后重试\",409)},a.onerror=function(){f"
		"ail(\"连接中断，请检查网线后重试\",0)},\"p4\"==t.id&&o.length){var"
		" d=hex($(\"[name=stockoff]\",t).value),c=$(\"[name=wipe]\",t);return"
		" a.open(\"POST\",\"/stock?off=0x\"+(d<0?0:d).toString(16)+(c&&c.checked?\"&wipe=1\":\"\")),void"
		" a.send(o[0].f)}a.open(\"POST\",\"/\"),a.send(n)}function fail(e,t){var"
		" o=SENT.p,n=$(\".prog\",o),a=$(\"button[type=submit]\",o);clearInterval(WRT),WRT=null,clearTimeout(W"
		"RP),WRP=null,WR&&(WR.fin=1),HBOFF=0,busy(0),n.className=\"prog"
		" bad\",$(\".pbar\",n).className=\"pbar\",$(\".pbar\",n).style.width=\"100%\",$(\".pwhat\",n).textContent=e,"
		"$(\".pct\",n).textContent=\"\",a.disabled=!1,a.textContent=SENT.btxt,\"p4\"==o.id&&(0===t||t>=500)&&(S"
		"TUCKN=null,STUCK=t>=500?\"<b>写入未完成，闪存已写入一部分。</b>在重新写入成功之前不要重启设备。\":\"<b>连接中断，写入可能未完成。</b>在重新写入成功之前不"
		"要重启设备。\",banner())}function done(e){var t=SENT.p;return\"p4\"==t.id?(HBOFF=0,void"
		" stdone(e)):WR&&WR.fin?void(HBOFF=0):$(\"[name=tryboot]\",t)&&$(\"[name=tryboot]\",t).checked?(HBOFF"
		"=0,clearInterval(WRT),WRT=null,WR&&(WR.fin=1),$(\"#steps\").innerHTML=\"<li>\"+plan().join(\"</li><li"
		">\")+\"</li>\",$(\"#ban\").setAttribute(\"hidden\",\"\"),TRYB=1,busy(0),expect(\"boot\"),void"
		" show(\"p7\")):void wrpoll()}function wrpoll(){WR&&!WR.fin&&get(\"/wr?from=\"+WR.off,function(e,t){i"
		"f(WR&&!WR.fin){var o=200==e&&t?t.indexOf(\"\\n\"):-1;if(o>0){var"
		" n=parseInt(t.slice(0,o),10);n>=WR.off&&(WR.off=n,WR.txt+=t.slice(o+1),wrline(WR.txt))}WR&&!WR.f"
		"in&&(++WR.k>1200?fail(\"未收到设备回报，请查看「诊断」中的串口日志\",500):WRP=setTimeout(wrpoll,700))}})}function"
		" wrline(e){var t,o;if(WR&&!WR.fin)for(;(t=e.indexOf(\"\\n\",WR.n))>=0;)o=e.slice(WR.n,t).replace(/\\"
		"r$/,\"\"),WR.n=t+1,o&&wrstep(o)}function wrstep(e){var"
		" t,o,n,a,i,s=$(\".prog\",SENT.p),r=$(\".pbar\",s),l=e.charAt(0),d=e.slice(2).split(\""
		" \");if(\"s\"==l)return t=+d[d.length-1],WR.left=0,t>0&&(d.pop(),WR.left=Math.max(2,Math.round(t/WR"
		"SPD))),\"回读校验\"==d[0]&&(WR.vf=d.slice(1).join(\" \")),r.className=\"pbar"
		" ind\",r.style.width=\"100%\",$(\".pwhat\",s).textContent=d.join(\""
		" \")+\"…\",void($(\".pct\",s).textContent=WR.left?\"预计 \"+dur(WR.left):\"\");if(\"v\"==l)return"
		" o=+d[0],n=+d[1],i=((a=Date.now())-WR.rt)/1e3,WR.left=0,r.className=\"pbar\",r.style.width=(n?o/n*"
		"100:0)+\"%\",i>=1.5&&(WR.rate=(o-WR.rn)/i,WR.rt=a,WR.rn=o),void($(\".pct\",s).textContent=sz(o)+\" /"
		" \"+sz(n)+\" · \"+(n?(o/n*100).toFixed(0):0)+\"%\"+(WR.rate>0?\" · \"+spd(WR.rate)+\" · 剩余"
		" \"+dur((n-o)/WR.rate):\"\"));if(\"r\"!=l){if(\"c\"==l)return\"ok\"==d[0]?void(WR.ver=\"通过\"):(STUCK&&!STUC"
		"KN||(STUCK||(STUCKN=[]),(WR.vf?[WR.vf]:WR.rows.map(function(e){return"
		" e.n})).forEach(function(e){STUCKN.indexOf(e)<0&&STUCKN.push(e)}),STUCK=\"<b>回读校验没通过，闪存里的内容与上传的不一"
		"致。</b>重新写入至成功之前不要重启设备。\"),banner(),void fail(\"回读校验没通过：\"+d.slice(1).join(\""
		" \"),500));if(\"t\"!=l)\"f\"!=l?\"done\"==e&&wrfin():fail(e.slice(2),500);else{if(t=+d[0],o=+d[1],t>0&&"
		"o>0){WRSPD=t/o;try{localStorage.setItem(\"xgwrspd\",String(Math.round(WRSPD)))}catch(e){}}WR.secs="
		"o}}else WR.rows.push({n:d[0],s:+d[1]||0,c:d[2]||\"\"})}function"
		" wrclock(){WR&&!WR.fin&&WR.left&&SENT&&(WR.left--,$(\".pct\",$(\".prog\",SENT.p)).textContent=WR.lef"
		"t>0?\"预计还需 \"+dur(WR.left):\"即将完成…\")}function wrfin(){var"
		" e=SENT.p,t=$(\".prog\",e),o=$(\".pbar\",t),n=$(\"button[type=submit]\",e),a=\"\";WR&&!WR.fin&&(WR.fin=1"
		",clearInterval(WRT),WRT=null,clearTimeout(WRP),WRP=null,HBOFF=0,busy(0),STUCK&&STUCKN&&((STUCKN="
		"STUCKN.filter(function(e){return!WR.rows.some(function(t){return"
		" t.n==e})})).length||(STUCK=\"\")),CHK=null,t.className=\"prog"
		" ok\",o.className=\"pbar\",o.style.width=\"100%\",$(\".pwhat\",t).textContent=\"写入完成\",$(\".pct\",t).textCo"
		"ntent=\"\",n.disabled=!1,n.textContent=SENT.btxt,clear(e),WR.rows.forEach(function(e){a+=\"<div"
		" class=r><span>\"+esc(e.n)+\"</span><span class=v>\"+sz(e.s)+(e.c?\" · crc32"
		" \"+esc(e.c):\"\")+\"</span></div>\"}),WR.ver&&(a+=\"<div class=r><span>回读校验</span><span"
		" class=v>\"+esc(WR.ver)+\"</span></div>\"),WR.secs&&(a+=\"<div class=r><span>用时</span><span"
		" class=v>\"+dur(WR.secs)+\"</span></div>\"),a||(a=\"<div"
		" class=r><span>设备已写入完成</span></div>\"),a+=\"<div class=w>闪存已写入。现在重启即以新内容启动；也可以留在本页面接着写别的。</div>\",I"
		"NFO&&INFO.ubi&&!INFO.ubi.fip&&!WR.rows.some(function(e){return\"U-Boot\"==e.n})&&(a+=\"<div"
		" class=e>闪存里仍然没有 U-Boot（fip 卷），此时重启将无法启动</div>\"),$(\"#rbody\").innerHTML=a,$(\"#rmask\").setAttribut"
		"e(\"data-on\",\"\"))}function rhide(){$(\"#rmask\").removeAttribute(\"data-on\"),info()}function"
		" rboot(){$(\"#rmask\").removeAttribute(\"data-on\"),doreboot()}function stdone(e){STUCK=\"\";var"
		" t,o=/ok (\\d+) bytes crc32 ([0-9a-f]+) skipped (-?\\d+)(?: wiped"
		" (\\d+))?/.exec(e||\"\"),n=\"\",a=INFO&&INFO.flash;o?(t=+o[3],n=\"<tr><td>写入</td><td>\"+sz(+o[1])+\"（\"+o"
		"[1]+\" 字节）</td></tr><tr><td>crc32</td><td class=mono>\"+o[2]+\"</td></tr><tr><td>坏块</td><td>\"+(t?\"跳"
		"过 \"+t+\" 块，镜像中对应内容未写入\":\"无\")+\"</td></tr>\"+(void 0===o[4]?\"\":\"<tr><td>擦净尾部</td><td>\"+(a?sz(+o[4]*a."
		"erase)+\"（\"+o[4]+\" 块）\":o[4]+\" 块\")+\"</td></tr>\")):n=\"<tr><td>回报</td><td>\"+esc((e||\"\").trim()||\"（无）"
		"\")+\"</td></tr>\",CHK=null,$(\"#stres\").innerHTML=n,$(\"#ban\").setAttribute(\"hidden\",\"\"),hbstop(),sh"
		"ow(\"p13\")}function clear(e){$$(\"input[type=file]\",e).forEach(function(e){e.value=\"\";var"
		" t=e.parentNode,o=$(\".fn\",t),n=$(\".fs\",t);t.classList.remove(\"has\"),o&&(o.classList.remove(\"has\""
		"),o.textContent=t.classList.contains(\"drop\")?\"选择固件文件\":\"未选择\"),n&&n.parentNode.removeChild(n)}),$$"
		"(\"input[type=text]\",e).forEach(function(e){\"stockoff\"!=e.name&&(e.value=\"\")}),refresh()}document"
		".addEventListener(\"keydown\",function(e){\"Escape\"==e.key&&hide()});var I18N={\"清空设置\":\"Clear"
		" settings\",\"清空系统设置\":\"Clear system settings\",\"删掉 OpenWrt 的设置和装过的软件包，固件与出厂数据不动\":\"Removes"
		" OpenWrt's settings and installed packages; firmware and factory data"
		" stay\",\"清空\":\"Clear\",\"已清空\":\"Cleared\",\"正在清空…\":\"Clearing…\",\"删除\":\"Delete\",\"OpenWrt"
		" 的设置和软件包全部清空，下次启动为全新系统\":\"All OpenWrt settings and packages are erased; the next boot starts"
		" fresh\",\"闪存上没有 UBI，没有可清空的设置\":\"No UBI on the flash, so there are no settings to clear\",\"没有"
		" rootfs_data 卷，下次启动就是全新系统\":\"There is no rootfs_data volume; the next boot starts"
		" fresh\",\"已清空。下次启动为全新系统\":\"Cleared. The next boot starts fresh\",\"下发网关\":\"Hand out a"
		" gateway\",\"下发\":\"Yes\",\"不下发\":\"No\",\"关掉后路由器不下发网关\":\"Off: the router hands out no"
		" gateway\",\"已打开。电脑重新插拔网线后生效\":\"On. Replug the computer's cable for it to take"
		" effect\",\"已关闭。电脑重新插拔网线后生效\":\"Off. Replug the computer's cable for it to take"
		" effect\",\"已打开，但未能保存到闪存。电脑重新插拔网线后生效\":\"On, but not saved to flash. Replug the computer's cable"
		" for it to take effect\",\"已关闭，但未能保存到闪存。电脑重新插拔网线后生效\":\"Off, but not saved to flash. Replug the"
		" computer's cable for it to take effect\",\"原厂系统请先整片备份 · \":\"Stock firmware: back up the whole"
		" flash first · \",\"去备份\":\"Back up now\",\"写入 固件…\":\"Writing firmware…\",\"回读校验 固件…\":\"Verifying"
		" firmware…\",\"文件名里没有机型\":\"No model in the file name\",\"应为 .itb 文件\":\"Expected a .itb file\",\"应为 .bin"
		" 文件\":\"Expected a .bin file\",\"应为 .fip 文件\":\"Expected a .fip file\",\"日常刷机\":\"Flash"
		" firmware\",\"引导升级\":\"Bootloader\",\"试跑固件\":\"Boot from RAM\",\"刷回原厂\":\"Restore stock\",\"按卷写入\":\"UBI"
		" volumes\",\"备份下载\":\"Backup\",\"设备详情\":\"Device info\",\"系统诊断\":\"Diagnostics\",\"环境变量\":\"Environment\",\"启动与重启\""
		":\"Boot & reboot\",\"关于\":\"About\",\"切换深浅色\":\"Toggle light / dark\",\"连接中…\":\"Linking…\",\"将 sysupgrade"
		" 固件写入 fit 卷。rootfs_data 将被清空；系统可正常启动时请使用 sysupgrade。\":\"Writes a sysupgrade image into the fit"
		" volume. rootfs_data is erased. While the system still boots, use sysupgrade"
		" instead.\",\"固件\":\"Firmware\",\"选择固件文件\":\"Choose a firmware file\",\"选择文件…\":\"Choose"
		" file…\",\"设备停在恢复模式，不会自行引导。写入开始前闪存不会被修改；写完回读校验一遍，再由你决定是否重启。\":\"The device is held in recovery and"
		" will not boot on its own. Nothing is written to flash until the upload finishes; the image is"
		" then read back and verified, and rebooting is your call.\",\"想先不写闪存试一次，去「\":\"To try an image"
		" without touching flash, go to \",\"未选择文件\":\"No file selected\",\"上传并刷写\":\"Upload and flash\",\"写入 BL2"
		" 与 U-Boot FIP。用于从 tcboot / 原厂布局首次迁移，或升级 U-Boot。\":\"Writes BL2 and the U-Boot FIP. Use it for the"
		" first migration from a tcboot / stock layout, or to upgrade U-Boot.\",\"未选择\":\"None"
		" selected\",\"可选，同时写入\":\"Optional, written in the same pass\",\"首次迁移\":\"First migration\",\"重建"
		" UBI\":\"Rebuild UBI\",\"擦除 ubi 分区并重新创建全部卷；出厂 MAC、U-Boot 环境与用户配置将丢失。\":\"Erases the ubi partition and"
		" recreates every volume. The factory MAC, the U-Boot environment and user settings are lost."
		" \",\"必须同时上传 BL2 与 U-Boot\":\"BL2 and U-Boot must both be uploaded\",\"重建 UBI 前先备份。\":\"Back up before"
		" rebuilding UBI. \",\"请先在「\":\"Export everything from \",\"」中导出。\":\""
		" first.\",\"」。\":\".\",\"。\":\".\",\"重建擦除的范围从 0x20000 起，盖住了原厂引导器的后半截\":\"The rebuild erases from 0x20000"
		" on, which covers the back half of the stock bootloader\",\"（原厂 bootloader 分区是 0x0–0x80000，而本布局的"
		" bl2 只占 0x0–0x20000）。所以只写 U-Boot、不写 BL2 的话，重启时原厂 BL2 会起来、却找不到它的下一级——只能拆串口救。因此这一项要求 BL2 与 U-Boot"
		" 一起传。\":\" (the stock bootloader partition is 0x0–0x80000, while bl2 in this layout only occupies"
		" 0x0–0x20000). So if you write U-Boot without BL2, the stock BL2 still starts at boot but can"
		" no longer find its next stage — only a serial console gets you out of that. Hence this option"
		" demands BL2 and U-Boot together.\",\"仅升级 U-Boot 时不需重建 UBI：只选择 U-Boot 文件，rootfs_data"
		" 保留。\":\"Upgrading U-Boot alone needs no rebuild: pick only the U-Boot file and rootfs_data"
		" survives.\",\"把恢复固件载入内存直接引导，\":\"Loads a recovery image into RAM and boots it. \",\"闪存一个字节都不写\":\"Not"
		" one byte of flash is written\",\"。起不来断电即恢复原系统。\":\". If it does not come up, power-cycle and the"
		" old system is back.\",\"选择恢复固件\":\"Choose a recovery image\",\"要用 initramfs 恢复固件。\":\"Use an initramfs"
		" recovery image. \",\"它的根文件系统随镜像一起进内存，不依赖闪存，所以 UBI 是空的、fit 卷没了也照样跑得起来。\":\"Its root filesystem"
		" travels into RAM with the image and needs no flash, so it runs even when UBI is empty or the"
		" fit volume is gone. \",\"sysupgrade 固件不适合这里\":\"A sysupgrade image will not do here\",\"：它的根要由"
		" fitblk 从闪存的 fit 卷里读（设备树的 rootdisk 指着那个卷），试跑时内核是新的、根还是闪存里那份旧的，多半起不来。\":\": its root is read by"
		" fitblk from the fit volume in flash (rootdisk in the device tree points there), so a trial run"
		" pairs a new kernel with the old root still in flash, and usually fails to"
		" boot.\",\"引导成功后本页面不再可用；要回到恢复页，断电重来即可。闪存没写过一个字节，原系统还在。\":\"Once it boots, this page is gone;"
		" power-cycle to get back to recovery. Flash was never written and the old system is"
		" intact.\",\"启动它\":\"Boot it\",\"将镜像写入 flash 指定偏移，边接收边写入、不在内存中暂存，因此没有单独的上传阶段。偏移为 0"
		" 时整片写入，当前引导程序与本页面将被覆盖；重新迁移至 OpenWrt 需经 USB-TTL 串口。\":\"Writes an image to a given flash offset."
		" It is written as it arrives rather than buffered in RAM, so there is no separate upload stage."
		" At offset 0 the whole chip is written, which overwrites the running bootloader and this page;"
		" migrating back to OpenWrt then needs a USB-TTL serial console.\",\"镜像\":\"Image\",\"写入偏移\":\"Write"
		" offset\",\"十六进制，按擦除块对齐\":\"Hexadecimal, aligned to an erase block\",\"0 为整片，亦可写入单个分区\":\"0 writes the"
		" whole chip; a single partition works too\",\"擦净尾部\":\"Erase the tail\",\"镜像之后\":\"The erase blocks"
		" after the image, \",\"直至片尾\":\"all the way to the end of the chip\",\"的擦除块一并擦空；不擦则保留原有内容\":\", are"
		" erased as well. Leave it off to keep what is there\",\"镜像须来自本机备份，可在「\":\"The image must come from"
		" a backup of this board, exported from \",\"」中导出。长度上限为 flash 容量 \":\". Its length is capped at the"
		" flash size, \",\"页面不校验镜像内容、机型与偏移是否匹配\":\"This page does not check that the image, the model and"
		" the offset match\",\"，写错仅能通过串口恢复。整片写入耗时显著长于固件写入；\":\", and a wrong write can only be undone over"
		" serial. A whole-chip write takes far longer than a firmware write."
		" \",\"写入一旦开始，中断将使闪存处于不一致状态，须重传至成功后方可重启\":\"Once writing starts, an interruption leaves flash"
		" inconsistent; resend until it succeeds before rebooting\",\"。写入期间指示灯\":\". The LEDs"
		" \",\"流水\":\"chase\",\"，与其他页面一致；本页写入与接收同时进行，\":\" while writing, as on the other pages. Here writing"
		" and receiving happen at once, so \",\"网线与电源都不能断\":\"neither the cable nor the power may be"
		" cut\",\"，进度以本页进度条为准。\":\". Go by the progress bar on this page.\",\"刷写\":\"Flash it\",\"写入 UBI 卷\":\"Write"
		" UBI volume\",\"按卷名写入 UBI 卷，卷不存在时按文件长度创建。物理位置由 UBI 层管理，不涉及 bl2 分区。\":\"Writes a UBI volume by name,"
		" creating it at the file's size if it does not exist. UBI decides where it physically lands;"
		" the bl2 partition is not involved.\",\"出厂数据\":\"Factory data\",\"文件须来自本机备份，可同时写入\":\"The files must"
		" come from a backup of this board; several can go in one pass\",\"任意卷\":\"Any"
		" volume\",\"卷名错误将覆盖对应卷的内容\":\"A wrong name overwrites whatever that volume holds\",\"卷名\":\"Volume"
		" name\",\"如 fip、fit\":\"e.g. fip, fit\",\"仅限字母、数字与 _ - .\":\"Letters, digits and _ - ."
		" only\",\"内容\":\"Contents\",\"卷\":\"Volume\",\"卷内容\":\"Volume"
		" contents\",\"写完由你决定是否重启，所以可以连着写好几个卷。\":\"Rebooting is your call once a write finishes, so several"
		" volumes can be written back to back.\",\"读取闪存内容并下载。读取与传输同步进行，不限长度；传输期间设备可能暂停响应。\":\"Reads flash"
		" and downloads it. Reading and transferring run together with no length limit; the device may"
		" stop answering while it does.\",\"UBI 卷\":\"Volumes\",\"原始区段\":\"Raw range\",\"正在读取…\":\"Reading…\",\"绕过 UBI"
		" 按 flash 偏移读取，偏移与「刷回原厂」一致。\":\"Reads by flash offset, bypassing UBI. The offsets match the ones"
		" on Restore stock.\",\"起始偏移\":\"Start offset\",\"十六进制\":\"Hexadecimal\",\"0 为片首\":\"0 is the start of the"
		" chip\",\"长度\":\"Length\",\"留空到片尾\":\"empty = to the end\",\"留空读至片尾\":\"Leave empty to read to the end of"
		" the chip\",\"整片读取的是闪存本身，不经过分区表，原厂布局下的 romfile、config 一并包含。\":\"A whole-chip read takes the flash"
		" itself, not the partition table, so on a stock layout romfile and config come along.\",\"文件偏移等于"
		" flash 偏移\":\"Offsets in the file equal flash offsets\",\"，坏块在文件中保留占位，格式与 \":\". Bad blocks keep"
		" their place in the file, and the format matches a \",\" 镜像一致：外部 \":\" image: an \",\""
		" 可直接写入，此处导出的镜像亦可用于编程器。写回时坏块跳过而不压缩，其后内容位置不变。\":\" from elsewhere can be written straight back, and"
		" an image exported here also works in a programmer. On the way back bad blocks are skipped"
		" rather than squeezed out, so everything after them stays put.\",\"整片下载\":\"Download whole"
		" chip\",\"下载区段\":\"Download range\",\"已完成\":\"Finished\",\"crc32 基于实际传出的字节计算，可与本地文件核对\":\"The crc32 covers"
		" the bytes actually sent, so it can be checked against the file you got\",\"选择卷，或填写偏移与长度\":\"Pick a"
		" volume, or fill in an offset and length\",\"传输期间设备不应答：U-Boot 的 TCP"
		" 栈同时只有一条连接，而这条连接正被下载占着，所以本页面问不到「传了多少字节」。\":\"The device goes quiet during the transfer: U-Boot's"
		" TCP stack holds one connection at a time and the download owns it, so this page cannot ask how"
		" many bytes have gone out. \",\"字节进度请看浏览器自己的下载栏\":\"Watch the browser's own download bar for"
		" that\",\"。传完这里会给出 crc32，可与本地文件核对。\":\". When it finishes, the crc32 appears here to check against"
		" the file.\",\"运行时从设备树、MTD 与 UBI 读取。\":\"Read at runtime from the device tree, MTD and"
		" UBI.\",\"硬件\":\"Hardware\",\"网络\":\"Network\",\"端口号为交换机内部顺序，与外壳丝印不一定对应；无响应的端口不列出。\":\"Port numbers follow"
		" the switch's internal order, which need not match the labels on the case. Ports that do not"
		" answer are left out.\",\"地址\":\"Address\",\"三种模式互斥，应用后需以新地址重新打开本页面\":\"The three modes are exclusive;"
		" after applying, reopen this page at the new address\",\"模式\":\"Mode\",\"DHCP 服务器 · 本机发地址\":\"DHCP"
		" server · this device hands out addresses\",\"静态地址\":\"Static address\",\"DHCP 客户端 · 向上级路由要\":\"DHCP"
		" client · ask the upstream router\",\"路由器 IP\":\"Router IP\",\"本机地址，末位固定为 1\":\"This device's address;"
		" the last octet is always 1\",\"子网掩码\":\"Subnet mask\",\"保存到闪存\":\"Save to"
		" flash\",\"不保存则只在本次开机有效\":\"Without this it only lasts until the next"
		" power-up\",\"应用\":\"Apply\",\"读取设备状态：关键位置检查、全片扫描，以及本次上电的控制台输出。\":\"Reads the device's state: a check"
		" of the critical spots, a whole-chip scan, and the console output since this"
		" power-up.\",\"快速检查\":\"Quick check\",\"全片扫描\":\"Full scan\",\"串口日志\":\"Serial log\",\"检查 BL2、坏块、UBI 各卷与"
		" U-Boot MAC。读取闪存期间设备暂停响应，耗时数秒。\":\"Checks BL2, bad blocks, the UBI volumes and the U-Boot MAC."
		" The device stops answering while it reads flash, for a few seconds.\",\"未执行检查\":\"Not checked"
		" yet\",\"点「开始检查」读取 BL2、坏块与各卷状态\":\"Press Run check to read BL2, the bad blocks and the state of"
		" each volume\",\"开始检查\":\"Run check\",\"复制诊断信息\":\"Copy diagnostics\",\"下载诊断包\":\"Download"
		" diagnostics\",\"未扫描\":\"Not scanned\",\"点「开始扫描」逐页读一遍整片闪存\":\"Press Start scan to read every page of"
		" the chip once\",\"逐页读取全片；只读，不改动闪存。「快速检查」只覆盖关键位置，颗粒退化需整片读取才能发现。分段执行，段间页面保持响应；整片耗时数分钟，其间设备响应变慢。\":\"R"
		"eads the whole chip page by page. Read-only; flash is not modified. Quick check only covers the"
		" critical spots, while a decaying NAND die shows up only in a full read. It runs in chunks and"
		" the page stays responsive between them; the whole chip takes minutes and the device answers"
		" more slowly meanwhile.\",\"开始扫描\":\"Start scan\",\"U-Boot 本次上电以来的控制台输出。\":\"U-Boot's console output"
		" since this power-up.\",\"未读取\":\"Not read yet\",\"日志缓冲已满，后续输出未记录。重启后重新开始记录。\":\"The log buffer is full"
		" and later output was not recorded. Recording restarts after a reboot.\",\"实时跟随\":\"Follow"
		" live\",\"读取日志\":\"Read log\",\"复制日志\":\"Copy log\",\"U-Boot 环境变量\":\"U-Boot"
		" environment\",\"当前生效的变量，只读。bootcmd 与引导菜单异常是无法启动的常见原因。\":\"The variables in effect, read-only. A"
		" broken bootcmd or boot menu is a common reason a device will not"
		" start.\",\"变量\":\"Variables\",\"引导菜单预览\":\"Boot menu preview\",\"恢复默认\":\"Restore"
		" defaults\",\"过滤名称或值\":\"Filter by name or value\",\"只看关键项\":\"Key variables only\",\"重新读取\":\"Read"
		" again\",\"复制全部\":\"Copy all\",\"设备启动时的菜单，由 bootmenu_* 变量构成。\":\"The menu shown at boot, built from the"
		" bootmenu_* variables.\",\"恢复为本版 U-Boot 的默认值\":\"Restore this U-Boot's own defaults\",\"清除自定义的"
		" bootcmd、引导菜单与网络设置并保存\":\"Clears a customised bootcmd, boot menu and network settings, and"
		" saves\",\"出厂 MAC 位于 ri 卷，\":\"The factory MAC lives in the ri volume and is \",\"不受影响\":\"left"
		" alone\",\"；固件与用户配置同样不受影响。恢复后需重启生效。UBI 无法挂载时保存失败，改动仅存于内存。\":\"; firmware and user settings are"
		" untouched as well. A reboot is needed for it to take effect. If UBI cannot be mounted the save"
		" fails and the change lives only in RAM.\",\"离开本页面的三种方式，均不改动闪存。\":\"Three ways to leave this page."
		" None of them touches flash.\",\"重启后正常引导，\":\"After a reboot the device boots normally, and"
		" \",\"引导成功就离开本页面了\":\"once it boots you have left this page\",\" ——"
		" 之后这个地址上是系统自己的页面。只有引导失败才回到恢复页，届时本页面自动刷新。\":\" — this address then belongs to the system's own"
		" page. Only a failed boot comes back to recovery, and this page reloads itself when it"
		" does.\",\"立即重启\":\"Reboot now\",\"其他启动方式\":\"Other ways to start\",\"直接启动系统\":\"Boot the system now\",\"执行"
		" bootcmd，不经过冷启动；引导失败回到本页面\":\"Runs bootcmd without a cold start; a failed boot returns to this"
		" page\",\"启动系统\":\"Boot system\",\"下次开机进恢复页\":\"Stop in recovery next boot\",\"仅生效一次，进入后自动还原\":\"One shot;"
		" it clears itself once you get here\",\"设置\":\"Arm\",\"设置后下次开机停在本页面，再下次开机恢复正常引导。\":\"Once armed, the"
		" next power-up stops on this page and the one after that boots normally again.\",\"U-Boot 内置的"
		" HTTP 恢复服务，页面不依赖任何外部资源。\":\"An HTTP recovery service built into U-Boot. The page loads nothing"
		" from the network.\",\"作者\":\"Author\",\"项目主页\":\"Project\",\"门户\":\"Portal\",\"问题反馈\":\"Issues\",\"基于\":\"Based"
		" on\",\"已交给设备启动\":\"Handed over to the device\",\"设备正在从内存引导该固件，闪存未改动。引导成功后本页面不再可用。\":\"The device is"
		" booting that image from RAM. Flash was not modified. Once it boots, this page is"
		" gone.\",\"起不来怎么办\":\"If it does not come up\",\"断电\":\"Power off\",\"再上电即回到原有系统，闪存一个字节都没写\":\"Power it"
		" back on and the old system is there; not one byte of flash was"
		" written\",\"等待\":\"Wait\",\"引导失败时设备自行回到本页面，届时自动刷新\":\"A failed boot brings the device back to this"
		" page, which then reloads itself\",\"本次动作\":\"What happens\",\"写入完成\":\"Write"
		" finished\",\"镜像已全部写入闪存，设备正在重启。本页面可以关闭。\":\"The whole image is in flash and the device is"
		" rebooting. You can close this page.\",\"设备回报\":\"Device reported\",\"crc32"
		" 由设备对收到的字节计算，与备份时记录的值一致即说明传输无误。\":\"The device computed the crc32 over the bytes it received; if"
		" it matches the one noted at backup time, the transfer was clean.\",\"重启约需 1–2 分钟。整片写入后本页面所属的"
		" U-Boot 已被覆盖，设备将按镜像中的引导程序启动。\":\"The reboot takes one or two minutes. After a whole-chip write"
		" the U-Boot this page belongs to is gone, and the device starts from the bootloader inside the"
		" image.\",\"重新连接\":\"Reconnect\",\"确认写入\":\"Confirm write\",\"取消\":\"Cancel\",\"仍要写入\":\"Write"
		" anyway\",\"写入完毕\":\"Write complete\",\"留在恢复页\":\"Stay in"
		" recovery\",\"已连接\":\"Online\",\"设备忙…\":\"Busy…\",\"无响应…\":\"Quiet…\",\"已断开\":\"Offline\",\"闪存中没有可挂载的"
		" UBI。\":\"There is no mountable UBI in flash. \",\"首次迁移：在「引导升级」中同时上传 BL2、U-Boot 与固件，并启用「重建"
		" UBI」。\":\"First migration: on Bootloader, upload BL2, U-Boot and the firmware together and turn"
		" on Rebuild UBI.\",\"闪存中没有 U-Boot（fip 卷）。\":\"There is no U-Boot in flash (the fip volume). \",\"当前"
		" U-Boot 仅存于内存，掉电丢失。请在「引导升级」中上传 U-Boot 文件。\":\"The running U-Boot lives only in RAM and is lost on"
		" power-off. Upload a U-Boot file on Bootloader.\",\"设备正在重启\":\"The device is"
		" rebooting\",\"引导成功即进入系统，\":\"A successful boot lands in the system and \",\"本页面不再可用\":\"this page is"
		" gone\",\"；引导失败才回到这里并自动刷新。\":\"; only a failed boot comes back here, and the page reloads"
		" itself.\",\"设备正在启动系统\":\"The device is booting the system\",\"闪存未改动。引导成功后\":\"Flash was not modified."
		" Once it boots, \",\"；引导失败则回到本页面。\":\"; a failed boot returns to this page.\",\"与设备的连接已断开\":\"The"
		" connection to the device is down\",\"设备无响应，请检查网线与电源。指示灯仍在流水说明它还活着，请稍候。\":\"The device is not"
		" answering. Check the cable and the power. If the LEDs are still chasing it is alive, so give"
		" it a moment.\",\"每 2 秒自动重试。\":\"Retrying every 2 seconds.\",\"正在重试…\":\"Retrying…\",\"闪存内容不受影响\":\"Flash"
		" is not affected\",\"闪存上没有可挂载的 UBI，当前 U-Boot 仅存于内存。\":\"There is no mountable UBI in flash and the"
		" running U-Boot lives only in RAM. \",\"重启后回到原有系统，需重新经串口传入 U-Boot"
		" 才能再打开本页面。建议先在「引导升级」中完成写入\":\"After a reboot the old system is back, and U-Boot has to be sent"
		" over serial again before this page can be opened. Better to finish the write on Bootloader"
		" first\",\"闪存中没有 U-Boot（fip 卷），当前 U-Boot 仅存于内存。\":\"There is no U-Boot in flash (the fip volume)"
		" and the running one lives only in RAM. \",\"重启后本页面将无法再打开\":\"After a reboot this page can no"
		" longer be opened\",\"重启\":\"Reboot\",\"动作\":\"Action\",\"DHCP 服务器\":\"DHCP server\",\"DHCP 客户端\":\"DHCP"
		" client\",\"设备的接口地址\":\"The device's interface address\",\"电脑直接插到本设备上时，插上就能拿到地址，不必手动配 IP。掩码固定 \":\"With"
		" a computer plugged straight into this device, it gets an address on connect and needs no"
		" manual IP. The mask is fixed at \",\"，电脑拿到的是本网段 \":\", the computer gets \",\"，不带 DNS。\":\" on this"
		" subnet, with no DNS. \",\"接入已有网络前不要用这一档\":\"Do not use this mode on an existing"
		" network\",\"：它对任何请求都应答，会和该网络的路由器抢着发地址，被抢到的机器会断网。\":\": it answers every request and races that"
		" network's router to hand out addresses, knocking whichever machines it wins off the"
		" network.\",\"接入已有网络时用这一档：填一个该网段内的空闲地址，本机不再发地址，应用后即可从网内任何一台机器打开本页面。\":\"Use this mode on an"
		" existing network: give it a free address on that subnet. The device stops handing out"
		" addresses, and once applied this page opens from any machine on the"
		" network.\",\"地址由上级路由分配，本页面事先不知道是多少，需在上级路由的客户端列表中按 MAC 查找。本机不再发地址。未取得租约时退回当前地址。\":\"The upstream"
		" router assigns the address, which this page cannot know in advance — look it up by MAC in the"
		" router's client list. The device stops handing out addresses. Without a lease it falls back to"
		" the current address.\",\"主机需在同一网段\":\"Your computer must be on the same subnet\",\"填本网段内任一地址即可，如"
		" 192.168.1.1\":\"Any address on this subnet will do, e.g. 192.168.1.1\",\"DHCP 服务器 192.168.1.1 /"
		" 255.255.255.0（出厂默认）\":\"DHCP server 192.168.1.1 / 255.255.255.0 (factory"
		" default)\",\"。勾上「保存到闪存」才会把当前这一套写进去替换它；不勾就只管本次开机。\":\". Only Save to flash writes the current"
		" settings over it; without it they last until the next power-up.\",\"IP 不是一个合法的地址\":\"That is not a"
		" valid IP address\",\"子网掩码不是一个合法的地址\":\"That is not a valid subnet mask\",\"改网络模式\":\"Change network"
		" mode\",\"本机\":\"This device\",\"不再发地址\":\"stops handing out addresses\",\"，电脑需手动配置同网段的 IP\":\", so the"
		" computer needs a manual IP on the same subnet\",\"地址由上级路由分配，本页面无法预知。\":\"The upstream router"
		" assigns the address; this page cannot know it. \",\"；未取得租约时退回当前地址\":\"; without a lease it falls"
		" back to the current address\",\"：下次开机就用这一套。地址填错时断电也无法恢复\":\": the next power-up uses these"
		" settings. If the address is wrong, a power-cycle will not undo it\",\"本机用的是设备发的地址\":\"Your"
		" computer is using an address this device handed out\",\"请改用上级路由那个网络里的机器\":\"use a machine on the"
		" upstream router's network instead\",\"正在获取地址\":\"Getting an address\",\"10 秒后自动跳转。\":\"Jumping there"
		" in 10 seconds.\",\"闪存内容不受影响；引导失败回到本页面\":\"Flash is not affected; a failed boot returns to this"
		" page\",\"未设置\":\"Not armed\",\"未保存\":\"Not saved\",\"已设置\":\"Armed\",\"已设置，但未能保存到闪存：断电后失效，请在「诊断」中确认 ubootenv"
		" 卷\":\"Armed, but it could not be saved to flash: it will not survive a power-off. Check the"
		" ubootenv volume under Diagnostics\",\"此前已设置，下次开机将停在本页面\":\"Already armed; the next power-up stops"
		" on this page\",\"已设置。下次开机停在本页面，再下次开机恢复正常引导\":\"Armed. The next power-up stops on this page, and"
		" the one after that boots normally again\",\"读取失败，刷新页面重试\":\"Read failed. Reload the page and try"
		" again\",\"UBI 未挂载\":\"UBI is not mounted\",\"首次迁移前闪存仍为原厂内容，建议此时用「原始区段」整片备份\":\"Before the first"
		" migration flash still holds the stock content — this is the moment to back up the whole chip"
		" from Raw range\",\"下载\":\"Download\",\"设备拒绝了本次备份\":\"The device refused this backup\",\"到片尾\":\"to the"
		" end\",\"未收到设备回报，本次备份可能未完成，请查看「诊断」中的串口日志\":\"No word back from the device — the backup may be"
		" incomplete. Check the serial log under Diagnostics\",\"传输完成\":\"Transfer"
		" complete\",\"无法读取闪存信息\":\"Cannot read the flash information\",\"起始偏移不是有效的十六进制数\":\"The start offset is"
		" not a valid hexadecimal number\",\"长度不是有效的十六进制数\":\"The length is not a valid hexadecimal"
		" number\",\"长度为 0\":\"The length is 0\",\"设备正在写入，稍后再试\":\"The device is writing. Try again"
		" shortly\",\"设备正忙：另一个上传还没结束，请稍后重试\":\"The device is busy with another upload. Try again once it is"
		" done\",\"没有匹配的变量\":\"No variable matches\",\"没有 bootmenu_* 条目\":\"No bootmenu_*"
		" entries\",\"环境中没有引导菜单：串口不会停顿，直接执行 bootcmd\":\"The environment has no boot menu: the console will"
		" not pause and bootcmd runs straight away\",\"自定义的 bootcmd、引导菜单与网络设置将恢复为本版 U-Boot 的默认值\":\"A"
		" customised bootcmd, boot menu and network settings go back to this U-Boot's defaults\",\"出厂"
		" MAC、固件与用户配置不受影响；恢复后需重启生效\":\"The factory MAC, the firmware and user settings are untouched; a"
		" reboot is needed for it to take effect\",\"正在恢复…\":\"Restoring…\",\"已恢复并保存，重启后生效\":\"Restored and"
		" saved; it takes effect after a reboot\",\"已恢复，但保存失败（UBI 未挂载），改动仅存于内存\":\"Restored, but the save"
		" failed (UBI is not mounted), so the change lives only in RAM\",\"读不到网络信息\":\"Cannot read the"
		" network information\",\"网关\":\"Gateway\",\"网卡\":\"Interface\",\"未发出过地址；当前主机用的是手动配置的 IP，更换主机后需重新配置。\":\"No"
		" address has been handed out; your computer is on a manually configured IP, and another machine"
		" would need configuring too.\",\"未连接\":\"No link\",\"机型\":\"Model\",\"内存\":\"RAM\",\"闪存\":\"Flash\",\"分区\":\"Partiti"
		"ons\",\"名称\":\"Name\",\"类型\":\"Type\",\"大小\":\"Size\",\"已用\":\"Used\",\"检查中…\":\"Checking…\",\"正在检查，读取闪存期间设备不响应…\":\"Che"
		"cking. The device does not answer while it reads flash…\",\"重新检查\":\"Check again\",\"重新扫描\":\"Scan"
		" again\",\"已停止\":\"Stopped\",\"已停止，结果仅覆盖已扫描的部分\":\"Stopped; the result only covers what was"
		" scanned\",\"停止\":\"Stop\",\"正在扫描\":\"Scanning\",\"扫描完成，存在无法读出的页\":\"Scan finished; some pages could not be"
		" read\",\"扫描完成\":\"Scan finished\",\"已扫描\":\"Scanned\",\"坏块\":\"Bad blocks\",\"无\":\"None\",\"ECC 纠错\":\"ECC"
		" corrections\",\"读失败\":\"Read failures\",\"扫描完成，见上表\":\"Scan finished; see the table"
		" above\",\"扫描完成，全片可读\":\"Scan finished; the whole chip reads\",\"已复制\":\"Copied\",\"(未运行健康检查)\":\"(no"
		" health check was run)\",\"导出于 \":\"Exported \",\"== 设备 ==\":\"== Device ==\",\"(/info 读取失败)\":\"(/info"
		" could not be read)\",\"== 网络 ==\":\"== Network ==\",\"(未读取)\":\"(not read)\",\"== 快速检查 ==\":\"== Quick"
		" check ==\",\"(未运行)\":\"(not run)\",\"== 环境变量 ==\":\"== Environment ==\",\"== 全片扫描 ==\":\"== Full scan"
		" ==\",\"(未扫描)\":\"(not scanned)\",\"== 串口日志 ==\":\"== Serial log"
		" ==\",\"正在收集…\":\"Collecting…\",\"将固件载入内存并直接引导\":\"Load the image into RAM and boot it\",\"不写入闪存\":\"Flash"
		" is not written\",\"；引导失败断电即恢复原系统\":\"; if it fails to boot, power-cycle and the old system is"
		" back\",\"试跑需要选择一个恢复固件\":\"A trial run needs a recovery image\",\"试跑仅接收固件本身，其他文件不会被写入\":\"A trial run"
		" takes the image alone; no other file is written\",\"固件仅载入内存，不写入闪存；引导失败断电即恢复原系统\":\"The image only"
		" goes into RAM, not flash; if it fails to boot, power-cycle and the old system is"
		" back\",\"这个文件名不像 initramfs 恢复固件。sysupgrade 固件的根文件系统在闪存的 fit 卷里，试运行不写闪存也就用不到它，内核多半起不来\":\"That file"
		" name does not look like an initramfs recovery image. A sysupgrade image keeps its root"
		" filesystem in the fit volume in flash, which a trial run never touches, so the kernel will"
		" most likely not come up\",\"写入偏移不是有效的十六进制数\":\"The write offset is not a valid hexadecimal"
		" number\",\"写入随上传同步进行，中断将使闪存处于不一致状态，须重传至成功后方可重启\":\"Writing runs alongside the upload; an"
		" interruption leaves flash inconsistent, so resend until it succeeds before"
		" rebooting\",\"写入后本页面不再可用，重新迁移需经串口\":\"After this write the page is gone, and migrating back needs"
		" a serial console\",\"选择了卷内容但未填写卷名\":\"Volume contents were picked but no volume name was"
		" given\",\"填写了卷名但未选择卷内容\":\"A volume name was given but no contents were picked\",\"卷名仅限字母、数字与 _ -"
		" .\":\"A volume name may only contain letters, digits and _ - .\",\"打开了「重建 UBI」却没有选择 U-Boot"
		" 文件：重建会抹掉 fip 卷，没有 U-Boot 设备将无法启动\":\"Rebuild UBI is on but no U-Boot file was picked: the"
		" rebuild wipes the fip volume, and without U-Boot the device will not start\",\"打开了「重建 UBI」却没有选择"
		" BL2：重建从 0x20000 起擦，盖住了原厂引导器的后半截，只写 U-Boot 的话重启起不来，只能拆串口救\":\"Rebuild UBI is on but no BL2 was"
		" picked: the rebuild erases from 0x20000 on, which covers the back half of the stock"
		" bootloader. Writing U-Boot alone leaves a device that will not boot, and only a serial console"
		" gets you out of that\",\"闪存里没有 U-Boot（fip 卷），本次必须同时上传 U-Boot 文件\":\"There is no U-Boot in flash"
		" (the fip volume), so a U-Boot file has to go in this time\",\"闪存里没有可挂载的 UBI：请打开「重建 UBI」，并同时上传"
		" BL2、U-Boot 与固件\":\"There is no mountable UBI in flash: turn on Rebuild UBI and upload BL2,"
		" U-Boot and the firmware together\",\"重建 UBI 将清除出厂 MAC、U-Boot 环境与用户配置\":\"Rebuilding UBI clears the"
		" factory MAC, the U-Boot environment and user settings\",\"未选择 BL2 或 U-Boot，本次只写入固件\":\"No BL2 or"
		" U-Boot was picked; only the firmware goes in this time\",\"闪存里没有可挂载的"
		" UBI，请先在「引导升级」里完成首次迁移\":\"There is no mountable UBI in flash — finish the first migration on"
		" Bootloader first\",\"闪存里没有 U-Boot（fip 卷）：写完不会自动重启，但重启之前要先到「引导升级」里补上 U-Boot 文件\":\"There is no"
		" U-Boot in flash (the fip volume). The write will not reboot on its own, but add a U-Boot file"
		" on Bootloader before you do reboot\",\"rootfs_data 将被清空\":\"rootfs_data will be"
		" erased\",\"未选择任何文件\":\"No files selected\",\"写入中…\":\"Writing…\",\"上传中…\":\"Uploading…\",\"传输完成，设备正在写入最后一块并擦净"
		"尾部…\":\"Transfer complete; the device is writing the last block and erasing the"
		" tail…\",\"传输完成，设备正在写入最后一块…\":\"Transfer complete; the device is writing the last"
		" block…\",\"已载入内存，即将启动\":\"Loaded into RAM, about to boot\",\"上传完成，设备开始写入闪存\":\"Upload complete; the"
		" device has started writing flash\",\"连接中断，请检查网线后重试\":\"The connection dropped. Check the cable and"
		" try again\",\"写入未完成，闪存已写入一部分。\":\"The write did not finish and flash is partly written."
		" \",\"在重新写入成功之前不要重启设备。\":\"Do not reboot the device until a write succeeds.\",\"连接中断，写入可能未完成。\":\"The"
		" connection dropped and the write may be incomplete. \",\"未收到设备回报，请查看「诊断」中的串口日志\":\"No word back"
		" from the device. Check the serial log under Diagnostics\",\"通过\":\"Passed\",\"回读校验没通过，闪存里的内容与上传的不一致。\""
		":\"The read-back check failed: what is in flash differs from what was uploaded."
		" \",\"重新写入至成功之前不要重启设备。\":\"Do not reboot until a rewrite succeeds.\",\"即将完成…\":\"Almost"
		" done…\",\"回读校验\":\"Read-back check\",\"用时\":\"Took\",\"设备已写入完成\":\"The device finished"
		" writing\",\"闪存已写入。现在重启即以新内容启动；也可以留在本页面接着写别的。\":\"Flash is written. Reboot now to start from it, or"
		" stay on this page and write something else.\",\"闪存里仍然没有 U-Boot（fip 卷），此时重启将无法启动\":\"There is still"
		" no U-Boot in flash (the fip volume); a reboot now will not"
		" start\",\"写入\":\"Written\",\"回报\":\"Reply\",\"（无）\":\"(none)\",\"重建 UBI…\":\"Rebuilding UBI…\",\"写入"
		" BL2…\":\"Writing BL2…\",\"写入 U-Boot…\":\"Writing U-Boot…\",\"写入 固件…\":\"Writing"
		" firmware…\",\"回读校验…\":\"Verifying…\",\"分区表\":\"Partition table\",\"可写空间\":\"Writable"
		" space\",\"引导\":\"Boot\",\"引导菜单\":\"Boot menu\",\"指示灯\":\"LEDs\",\"流水灯\":\"Chase LEDs\",\"环境\":\"Env"
		" volumes\",\"磨损\":\"Wear\",\"没有找到闪存设备\":\"No flash device found\",\"没有分区，只能按 flash 偏移读写\":\"No partitions;"
		" only raw flash-offset access works\",\"bl2 分区为空。断电后 BootROM 无法找到 BL2，请在「引导升级」页上传\":\"The bl2"
		" partition is empty. After a power-off the BootROM cannot find BL2 — upload one on"
		" Bootloader\",\"未设置，设备不会自行引导。可在「环境变量」页恢复默认值\":\"Not set, so the device will not boot on its own."
		" Restore the defaults on Environment\",\"与当前版本默认值一致\":\"Matches this version's default\",\"无条目\":\"No"
		" entries\",\"无法挂载，闪存上无可用的 UBI。首次迁移请在「引导升级」页启用「重建 UBI」，并同时上传 BL2、U-Boot 与固件\":\"Cannot be mounted;"
		" there is no usable UBI in flash. For a first migration, turn on Rebuild UBI on the Bootloader"
		" page and upload BL2, U-Boot and the firmware together\",\"已挂载但无法访问\":\"Mounted but not"
		" accessible\",\"UBI 已挂载但无法访问\":\"UBI is mounted but not accessible\",\"UBI 无法挂载，只能按 flash 偏移备份\":\"UBI"
		" cannot be mounted; only raw flash-offset backups are possible\",\"不存在。闪存中无"
		" U-Boot，断电后无法启动，请在「引导升级」页上传 U-Boot 文件\":\"Missing. There is no U-Boot in flash, so the device"
		" will not start after a power-off — upload a U-Boot file on"
		" Bootloader\",\"不存在。闪存中无固件，请在「日常刷机」页上传\":\"Missing. There is no firmware in flash — upload one on"
		" Flash firmware\",\"无法读取镜像描述，仅能确认为 FIT 格式\":\"The image description cannot be read; only the FIT"
		" format could be confirmed\",\"镜像中无描述信息\":\"The image carries no description\",\"无法读取，卷不存在\":\"Cannot"
		" be read; the volume does not exist\",\"存在，无法读取内容\":\"Present, but the contents cannot be"
		" read\",\"不存在。U-Boot 环境无处保存，首次正常启动时自动创建\":\"Missing. The U-Boot environment has nowhere to live; it"
		" is created at the first normal boot\",\"已读取\":\"Read OK\",\"已读取，内容为空\":\"Read OK, but empty\",\"已读取，无有效"
		" MAC\":\"Read OK, but no valid MAC in it\",\"ethaddr 不是有效地址\":\"ethaddr is not a valid address\",\"未启用"
		" CONFIG_LED，救砖时指示灯不流水\":\"CONFIG_LED is off, so the LEDs will not chase during"
		" recovery\",\"另一个备份正在传输，请等待传输完成后重试\":\"Another backup is transferring. Wait for it to finish and"
		" try again\",\"正在写入闪存，这一项要等写完再改\":\"Flash is being written; this one has to wait until the write is"
		" done\",\"设备正在传输备份。请等待传输完成后再上传，或刷新页面放弃该次下载\":\"The device is transferring a backup. Wait for it to"
		" finish before uploading, or reload the page to abandon that"
		" download\",\"设备正在接收上传，请等待写入完成后再备份\":\"The device is receiving an upload. Wait for the write to"
		" finish before backing up\",\"设备正忙，等这次传输完成后再扫描\":\"The device is busy; scan again once this"
		" transfer is done\",\"卷名不合法\":\"Invalid volume name\",\"卷名缺失或过长\":\"The volume name is missing or too"
		" long\",\"没有 Content-Length\":\"No Content-Length\",\"读取失败，详见串口日志\":\"Read failed — see the serial"
		" log\",\"起始偏移超过闪存容量\":\"The start offset is past the flash size\",\"偏移加长度超过闪存容量\":\"Offset plus length"
		" is past the flash size\",\"内存不足，无法分配读取窗口\":\"Not enough memory for a read window\"},I18P={\" 未备份 ·"
		" \":\" not backed up · \",\"与本机一致\":\"matches this device\",\"本机是 \":\"this device is \",\"。宽度为各卷的预留容量，按卷"
		" ID 排列；UBI 卷在闪存中并不连续，此图不表示物理位置\":\". Widths are each volume's reserved size, ordered by volume"
		" ID. UBI volumes are not contiguous in flash, so this is not a physical layout\",\" 个，其中 bl2"
		" 分区所在块已损坏，BootROM 可能无法读取 BL2：\":\" bad, including the block the bl2 partition sits in — the"
		" BootROM may fail to read BL2: \",\" MiB；写入固件时会先删掉 fit 与 rootfs_data，再腾出 \":\" MiB; writing"
		" firmware deletes fit and rootfs_data first, freeing another"
		" \",\"。闪存已写入一部分，此时重启将无法启动。请重新写入至成功，其间不要断电\":\". Flash is partly written and a reboot now will not"
		" start. Write again until it succeeds, and keep the power on\",\" 个块读取失败，该部分以 0xff"
		" 填充，详见「诊断」中的串口日志\":\" block(s) could not be read and were filled with 0xff — see the serial log"
		" under Diagnostics\",\"U-Boot 设备树未在 /options/u-boot 声明 \":\"The U-Boot device tree does not declare"
		" \",\"设备正在传输备份。请等待传输完成后再上传，或刷新页面放弃该次下载\":\"The device is transferring a backup. Wait for it to"
		" finish before uploading, or reload the page to abandon that"
		" download\",\"获取成功后设备位于新地址，请在上级路由的客户端列表中按 \":\"Once it has one, the device is at a new address —"
		" look it up in the upstream router's client list by \",\"，加上写入时会腾出的 fit 与 rootfs_data\":\", plus"
		" the fit and rootfs_data freed on write\",\" 不同。重建 UBI 后常见，将备份的出厂卷写回即可\":\" in the factory data."
		" Common after a UBI rebuild — just write the backed-up factory volume back\",\" 处无 BL2"
		" 镜像头，可能为原厂或第三方引导程序\":\" holds no BL2 image header — it may be a stock or third-party"
		" bootloader\",\" 不同。两份环境内容不一致，启动时使用较新的一份\":\" — the two copies differ, and the newer one is used at"
		" boot\",\"环境中无 ethaddr，本次启动使用随机地址 \":\"No ethaddr in the environment; this boot uses the random"
		" address \",\"，超过 bl2 分区自 0x800 起可写的 \":\", more than the bl2 partition can take from 0x800 on:"
		" \",\"请在上级路由的客户端列表中按 MAC 查找：\":\"Look it up by MAC in the upstream router's client list:"
		" \",\"。当前打开本页面的主机用的就是设备发的地址。\":\". The machine viewing this page is on one of"
		" them.\",\"少量属正常；成片出现表明颗粒退化，应尽快备份\":\"A few are normal; a cluster of them means the die is"
		" decaying, so back up soon\",\"，覆盖 bootloader 及其后全部内容\":\", overwriting the bootloader and"
		" everything after it\",\"，接近 SLC 颗粒常见寿命，需留意坏块增长\":\", near the usual life of an SLC die — watch for"
		" new bad blocks\",\"本页面连接的是旧地址，不会自动恢复。请以 \":\"This page is still on the old address and will not"
		" recover by itself. Reopen it at \",\"环境中没有这一项，当前 U-Boot 为 \":\"Not in the environment; the running"
		" U-Boot is \",\"。引导菜单为旧版本，下次正常启动时自动刷新\":\". The boot menu is from an older version and refreshes at"
		" the next normal boot\",\" 个块读取失败，该部分以 0xff 填充\":\" block(s) could not be read and were filled with"
		" 0xff\",\"。ECC 无法纠正，这些位置的数据已丢失\":\". ECC could not fix them, so the data there is"
		" lost\",\"。不一定装得下一份固件；可先删掉不用的卷\":\". That may not fit a firmware image; delete volumes you do not"
		" need\",\"另一个备份正在传输，请等待传输完成后重试\":\"Another backup is transferring. Wait for it to finish and try"
		" again\",\"设备正在接收上传，请等待写入完成后再备份\":\"The device is receiving an upload. Wait for the write to finish"
		" before backing up\",\"偏移加镜像长度超过 flash 容量 \":\"Offset plus image length is past the flash size,"
		" \",\"写入 U-Boot 失败，详见串口日志\":\"Writing U-Boot failed — see the serial log\",\"没有分区，只能按 flash 偏移读写\":\"No"
		" partitions; only raw flash-offset access works\",\"不保存：只管本次开机，下次开机回到 \":\"Not saved: this lasts"
		" until the next power-up, which goes back to \",\"设备启动时的菜单，序号即串口上按的键\":\"The menu shown at boot;"
		" the number is the key pressed on the serial console\",\"本页面将断开，需以 http://\":\"This page will drop;"
		" reopen it at http://\",\"偏移加长度超过 flash 容量 \":\"Offset plus length is past the flash size, \",\""
		" 秒内无按键则执行 bootcmd\":\" s without a keypress runs bootcmd\",\"），请重新上传 U-Boot 文件\":\"). Upload the"
		" U-Boot file again\",\"），卷内数据校验未通过，内容已损坏\":\"), and the volume's checksum failed — the contents are"
		" corrupt\",\"没有 Content-Length\":\"No Content-Length\",\"：本机这个地址马上就没人续租了，\":\": nothing will renew your"
		" computer's address any more, so \",\" 查找。未取得租约时退回原地址。\":\". Without a lease it falls back to the"
		" old address.\",\"起始偏移超出 flash 容量 \":\"The start offset is past the flash size, \",\"当前主机用的是手动配置的"
		" IP。\":\"your computer is on a manually configured IP.\",\"与当前版本默认值不同，已被修改：\":\"Differs from this"
		" version's default; modified: \",\"。下次正常启动时自动刷新引导菜单\":\". The boot menu refreshes at the next"
		" normal boot\",\"写入 BL2 失败，详见串口日志\":\"Writing BL2 failed — see the serial log\",\"挂载 UBI"
		" 失败，详见串口日志\":\"Mounting UBI failed — see the serial log\",\"重建 UBI 失败，详见串口日志\":\"Rebuilding UBI"
		" failed — see the serial log\",\"内存不足以分配接收环，至少需要 \":\"Not enough memory for the receive ring; at"
		" least \",\"正在写入闪存，这一项要等写完再改\":\"Flash is being written; this one has to wait until the write is"
		" done\",\" 字节。固件不完整，请重新上传\":\" bytes. The firmware is incomplete — upload it again\",\"本机开始发地址：电脑将拿到"
		" \":\"This device starts handing out addresses; the computer will get \",\" · 已截断，完整内容见串口\":\" ·"
		" truncated; the full list is on the serial console\",\"卷内不是 FIT 镜像（头 \":\"The volume does not hold"
		" a FIT image (header \",\"，与当前 U-Boot 一致\":\", matches the running U-Boot\",\" 读回来的内容与上传的不一致\":\" read"
		" back differently from what was uploaded\",\"自 flash 偏移 0x\":\"Writes from flash offset 0x\",\""
		" 块，镜像中对应内容未写入\":\" block(s); the matching parts of the image were not written\",\" 个，已由 UBI 避开：\":\""
		" bad, all avoided by UBI: \",\"，当前 U-Boot 为 \":\", the running U-Boot is \",\"写入固件失败，详见串口日志\":\"Writing"
		" the firmware failed — see the serial log\",\"整片写入失败，详见串口日志\":\"The whole-chip write failed — see"
		" the serial log\",\"内存不足，无法分配读取窗口\":\"Not enough memory for a read window\",\" 不存在时按文件长度创建\":\" will be"
		" created at the file's size if it does not exist\",\"，超过设备单次可接收的 \":\", more than the device can"
		" take in one go: \",\" 字节，超过它们覆盖的 \":\" bytes, more than the \",\"（本次读取已纠正位翻转）\":\" (bit flips were"
		" corrected on this read)\",\" 次地址，均未被接受；\":\" offer(s), none accepted; \",\"空闲与 UBI 预留 \":\"Free and"
		" UBI reserve \",\"，超过 fip 卷的 \":\", more than the fip volume's \",\"：内存不足，设备将拒绝\":\" — not enough RAM,"
		" so it will be refused\",\" 字节没有划进任何分区\":\" bytes not covered by any partition\",\"bl2"
		" 分区读取失败（\":\"Reading the bl2 partition failed (\",\" 项，当前版本默认为 \":\" entries; this version defaults"
		" to \",\"卷内不是 FIP（头 \":\"The volume does not hold a FIP (header \",\" 个灯，一个也没有找到\":\" LEDs and not one"
		" was found\",\" 卷失败，详见串口日志\":\" volume failed — see the serial log\",\" 字节将超出闪存容量 \":\" bytes would run"
		" past the flash size of \",\"读取失败，详见串口日志\":\"Read failed - see the serial"
		" log\",\"偏移加长度超过闪存容量\":\"Offset plus length is past the flash size\",\"设备拒绝了本次备份：\":\"The device"
		" refused this backup: \",\"。红色条目会写入闪存\":\". Red entries write to flash\",\" MiB：当前空闲 \":\" MiB — free"
		" now \",\" 页在读取时被纠正。\":\" page(s) were corrected on read. \",\"写入偏移未按擦除块 \":\"The write offset is not"
		" aligned to the erase block, \",\"U-Boot 文件 \":\"The U-Boot file is \",\" 处有 BL2 镜像\":\" holds a BL2"
		" image\",\"，救砖时指示灯不流水\":\" under /options/u-boot, so the LEDs will not chase during"
		" recovery\",\"起始偏移超过闪存容量\":\"The start offset is past the flash size\",\"需手动把本机配成 \":\"set it manually"
		" to an address in \",\"，最近一次分配给 \":\", most recently to \",\" 保留原有内容不动\":\" after the image keeps what"
		" is already there\",\" 的分区，无法检查\":\" among the partitions, so it cannot be checked\",\"），请重新上传固件\":\")."
		" Upload the firmware again\",\"，与出厂数据中的 \":\", differs from \",\"DHCP 服务器\":\"DHCP server\",\"DHCP"
		" 客户端\":\"DHCP client\",\"正在读取并传送 \":\"Reading and sending \",\" · 写入时腾出\":\" · freed on write\",\"镜像之后剩余的"
		" \":\"The \",\"回读校验没通过：\":\"The read-back check failed: \",\"设备拒绝了上传（\":\"The device refused the upload"
		" (\",\"声明的大小合计 \":\"declared sizes total \",\" MiB 未划分\":\" MiB is unpartitioned\",\" 个逻辑擦除块）\":\" logical"
		" erase blocks)\",\" 字节，无法读取\":\" bytes and cannot be read\",\" 字节，卷内仅 \":\" bytes but the volume holds"
		" only \",\" 字节，校验通过\":\" bytes, checksum OK\",\"，与出厂数据一致\":\", matches the factory data\",\"已读取，MAC"
		" \":\"Read OK, MAC \",\"（闪存尚未改动）\":\" (flash was not modified)\",\"没有找到闪存设备\":\"No flash device found\",\""
		" 个文件 · \":\" file(s) · \",\"设备没有接受：\":\"The device did not accept it: \",\"下次开机回到 \":\"The next power-up"
		" goes back to \",\"UBI 预留 \":\"UBI reserve \",\" · 擦除块 \":\" · erase block \",\" 项异常 · \":\" fail · \",\" 项注意"
		" · \":\" warn · \",\"扫描没能进行（\":\"The scan could not run (\",\" 将被擦成空白\":\" after the image will be erased"
		" blank\",\"BL2 文件 \":\"The BL2 file is \",\"设备写入失败（\":\"The device failed to write (\",\" 个卷，坏块 \":\""
		" volumes, \",\" 个逻辑擦除块\":\" free logical erase blocks\",\"擦写次数最大 \":\"Erase count max \",\"FIT 镜像，\":\"FIT"
		" image, \",\"存在，CRC \":\"Present, CRC \",\" 个没有找到）\":\" not found)\",\" 未按擦除块 \":\" is not aligned to the"
		" erase block \",\"卷名缺失或过长\":\"The volume name is missing or too long\",\"（出厂默认）\":\" (factory"
		" default)\",\" · 电脑 \":\" · computer \",\"/ 重新打开\":\"/\",\" 的一个地址\":\"\",\"设备已移至 \":\"The device moved to \",\""
		" 重新打开。\":\".\",\" 块读取失败\":\" unreadable block(s)\",\"空闲与预留 \":\"Free and reserve \",\"逻辑擦除块 \":\"Logical"
		" erase block \",\"。刷机可用 \":\". Available for flashing: \",\" · 剩余 \":\" · left \",\" 页无法读出\":\" page(s)"
		" could not be read\",\"连续覆盖整片\":\"contiguous over the whole chip\",\" 个，空闲 \":\" bad blocks, \",\"。当前空闲"
		" \":\". Free now \",\"，实际写入 \":\", wrote \",\"请求头超过 \":\"The request header is longer than"
		" \",\"下次开机：\":\"Next power-up: \",\"设置失败（\":\"Arming failed (\",\"读至片尾，\":\"Reads to the end of the chip,"
		" \",\"传输完成；\":\"Transfer complete; \",\" 起的区段\":\" onwards\",\"恢复失败（\":\"Restore failed (\",\" · 页 \":\" · page"
		" \",\"（擦除块 \":\" (erase block \",\"扫描完成，\":\"Scan finished; \",\"读取失败（\":\"Read failed (\",\" 起写入 \":\""
		" onwards: \",\"本次上传 \":\"This upload is \",\"正在写入 \":\"Writing \",\"正在上传 \":\"Uploading \",\"预计还需 \":\"About"
		" \",\"回读校验 \":\"Verifying \",\"没有名为 \":\"There is nothing named \",\" 个分区，\":\" partitions; \",\" 之间有 \":\" has"
		" \",\"连续覆盖 \":\"contiguous over \",\"片上其余 \":\"the chip's other \",\"，擦除块 \":\", erase block \",\"，OOB \":\","
		" OOB \",\"刷机可用 \":\"Available for flashing: \",\"闪存中为 \":\"Flash holds \",\"镜像声明 \":\"The image declares"
		" \",\"存在，与 \":\"Present, matches \",\" 声明了 \":\" declares \",\"（声明了 \":\" (declares \",\" 个，有 \":\", \",\"写入偏移"
		" \":\"The write offset \",\"卷名不合法\":\"Invalid volume name\",\"长度为 0\":\"The length is 0\",\"静态地址\":\"Static"
		" address\",\"已发出 \":\"Handed out \",\" 个地址\":\" address(es)\",\" 全双工\":\" full duplex\",\" 半双工\":\" half"
		" duplex\",\" 项正常\":\" ok\",\" 字节）\":\" bytes)\",\"：有重叠\":\" they span: they overlap\",\"擦除块 \":\"erase block"
		" \",\" 失败（\":\" failed (\",\"读不回 \":\"could not read back \",\"已选 \":\"Selected \",\"设备 \":\"Device \",\"端口"
		" \":\"Port \",\"空闲 \":\"Free \",\"全部 \":\"All \",\" 个：\":\": \",\" 页：\":\" page(s): \",\" 对齐\":\"\",\"预计 \":\"About \",\"跳过"
		" \":\"Skipped \",\" 块）\":\" block(s))\",\"写入 \":\"Writing \",\" 的卷\":\" among the volumes\",\"，页 \":\", page"
		" \",\"平均 \":\"average \",\"卷内 \":\"The volume holds \",\" 一致\":\"\",\"，与 \":\", differs from \",\" 的 \":\"'s \",\"擦除"
		" \":\"Erasing \",\" 字节\":\" bytes\",\" 卷\":\" volume\",\" 项\":\" entries\",\"卷 \":\"Volume \",\" 块\":\" block(s)\",\"页"
		" \":\"page \",\"自 \":\"From \",\"）：\":\"): \",\"；\":\"; \",\"、\":\", \",\"（\":\" (\",\"）\":\")\",\"，\":\","
		" \"},LANG=\"zh\",I18RE=null,I18R=null,I18ON=0,I18A=[\"title\",\"aria-label\",\"placeholder\",\"data-l\"],CJ"
		"K=/[\\u4e00-\\u9fff]/;function i18re(){var e,t=[];for(e in"
		" I18P)t.push(e);t.sort(function(e,t){return t.length-e.length}),I18RE=t.length?new"
		" RegExp(t.map(function(e){return e.replace(/[.*+?^${}()|[\\]\\\\]/g,\"\\\\$&\")}).join(\"|\"),\"g\"):null}f"
		"unction T(e){return\"en\"==LANG?i18s(e):e}function i18s(e){var t=I18N[e];return void"
		" 0!==t?t:I18RE?e.replace(I18RE,function(e){return I18P[e]}):e}function i18text(e){var"
		" t,o=e.data;o&&CJK.test(o)&&(t=i18s(o))!=o&&(void 0===e.zh0&&(e.zh0=o),e.data=t)}function"
		" i18attr(e,t){var o,n=e.getAttribute&&e.getAttribute(t);n&&CJK.test(n)&&(o=i18s(n))!=n&&(void"
		" 0===e[\"zh0_\"+t]&&(e[\"zh0_\"+t]=n),e.setAttribute(t,o))}function i18skip(e){var"
		" t=e.tagName;return\"SCRIPT\"==t||\"STYLE\"==t||e.hasAttribute(\"data-raw\")}function i18on(e){var"
		" t;if(3!=e.nodeType){if(1==e.nodeType&&!i18skip(e))for(I18A.forEach(function(t){e.hasAttribute(t"
		")&&i18attr(e,t)}),t=e.firstChild;t;t=t.nextSibling)i18on(t)}else i18text(e)}function"
		" i18rev(){for(var e in I18R={},I18N)I18R[I18N[e]]=e}function i18off(e){var"
		" t,o;if(3!=e.nodeType){if(1==e.nodeType&&!i18skip(e))for(I18A.forEach(function(t){var"
		" o=\"zh0_\"+t;void 0!==e[o]&&(e.setAttribute(t,e[o]),e[o]=void"
		" 0)}),t=e.firstChild;t;t=t.nextSibling)i18off(t)}else void 0!==e.zh0?(e.data=e.zh0,e.zh0=void"
		" 0):I18R&&void 0!==(o=I18R[e.data])&&(e.data=o)}function"
		" i18inraw(e){for(e=e.parentNode;e&&1==e.nodeType;e=e.parentNode)if(i18skip(e))return!0;return!1}"
		"var I18OB=window.MutationObserver?new MutationObserver(function(e){var"
		" t,o,n;if(I18ON)for(t=0;t<e.length;t++)if(\"characterData\"==(n=e[t]).type)i18inraw(n.target)||i18"
		"text(n.target);else if(\"attributes\"==n.type)i18attr(n.target,n.attributeName);else"
		" if(!i18inraw(n.addedNodes[0]||n.target))for(o=0;o<n.addedNodes.length;o++)i18on(n.addedNodes[o]"
		")}):null;function setlang(e){if($$(\".lg button\").forEach(function(t){t.setAttribute(\"aria-checke"
		"d\",t.getAttribute(\"data-g\")==e?\"true\":\"false\")}),e!=LANG){LANG=e;try{localStorage.setItem(\"xglan"
		"g\",e)}catch(e){}document.documentElement.setAttribute(\"lang\",e),\"en\"==e?(I18RE||i18re(),I18ON=1,"
		"i18on(document.body),I18OB&&I18OB.observe(document.body,{childList:!0,subtree:!0,characterData:!"
		"0,attributes:!0,attributeFilter:I18A})):(I18ON=0,I18OB&&I18OB.disconnect(),I18R||i18rev(),i18off"
		"(document.body))}}try{\"en\"==(localStorage.getItem(\"xglang\")||(/^zh/i.test(navigator.language||\"\""
		")?\"zh\":\"en\"))&&setlang(\"en\")}catch(e){}bind(),info(),hbstart();\n"
	"</script>\n"
	"</body></html>\n"
/* @@PAGE_END@@ */
	;

/* Sent once the upload has been staged; the page switches views on it. */
static const char resp_ok[] =
	"HTTP/1.0 200 OK\r\n"
	"Content-Type: application/json\r\n"
	"Connection: close\r\n"
	"\r\n"
	"{\"ok\":1}";

/* 400 with the reason as the body, which the page shows verbatim. */
static char resp_reject[512];

/*
 * ---- static response buffers ---------------------------------------------
 *
 * Every GET endpoint below builds its whole answer once, when the request is
 * classified, into a static buffer of its own -- info_buf, check_buf,
 * scan_buf, log_buf, ping_buf, env_buf, netmode_buf, dumpinfo_buf -- and then
 * hands tx() a pointer into that buffer for as long as the connection lives.
 *
 * That is sound only because of two properties of the U-Boot TCP stack.
 * net/tcp.c holds exactly one "static struct tcp_stream tcp_stream" and
 * refuses a new SYN while the old stream is not CLOSED, so two connections
 * are never alive at the same time; and a buffer is not rebuilt until the
 * next request is classified, by which time the previous answer has been
 * acknowledged in full -- on_snd_una_update() is what closes the stream.
 *
 * So if that stack ever grows a second stream, this is the first thing that
 * breaks, and it breaks quietly: one request would be served the tail of
 * another's reply.  Each endpoint would then need its buffer per stream,
 * hung off tcp->priv rather than off the file.
 */

/* GET /info: JSON built per request, at most this long. */
static char info_buf[4096];

/*
 * Who is holding the device.  These two live up here rather than each
 * with its own section because every long operation has to look at the
 * other: a scan while a download streams would stall it for seconds, and
 * a download during a write would read half-written flash.
 */
static int	dump_busy;	/* a download is streaming */

/* State of the single in-flight upload. */
static int	up_active;	/* a POST connection has been accepted */
static int	up_parsed;	/* headers understood, offsets known */
static int	up_failed;	/* parse error, answer 400 */
static int	up_ready;	/* image complete and moved to $loadaddr */
static u32	up_body;	/* stream offset of the first image byte */
static u32	up_total;	/* total request length (headers + body) */
static char	up_bound[80];	/* "\r\n--" + multipart boundary */
static struct tcp_stream *up_owner;	/* owns the staging area */
static u32	up_hi;		/* end of the furthest bytes staged */
static int	up_bound_len;
static int	flash_pending;	/* flash after net_loop() returns */
/*
 * Boot the uploaded image straight out of memory instead of writing it.
 * The whole point is that flash is not touched: an image that does not
 * come up costs a power cycle, not a recovery.
 */
static int	tryboot_pending;

/*
 * One uploaded form field whose payload is non-empty.  The name buffer has to
 * hold the longest field this file looks for, which is FIELD_FVOL_PREFIX plus
 * a full-length UBI volume name: anything shorter would cut the name silently
 * and fvol_find() would then never match the volume the user picked.
 */
struct up_part {
	char	name[sizeof(FIELD_FVOL_PREFIX) + UBIVOL_NAME_MAX];
	ulong	addr;
	u32	size;
};

static struct up_part	up_parts[MAX_PARTS];
static int		up_nparts;

/* Set from the form during httpd_flash(); read once it has returned. */
static int		flash_stay;

/*
 * ---- writing, one step at a time -----------------------------------
 *
 * The write used to happen after the answer had gone out and the
 * connection had closed, and it ended in a reset.  So the page could
 * only ever say "uploaded" -- never "written", never "and it reads back
 * the same", and never "do you want to reboot?".
 *
 * Now the write is a state machine taking one step per pass of
 * httpd_tick(), net_loop()'s timeout handler, so the page can ask how far
 * it got and the board stays up at the end instead of resetting itself.
 * That handler is the one clock that keeps going whether or not a browser
 * is still listening -- close the tab mid-write and the write still
 * finishes.  It also means the whole write happens inside the one
 * net_loop() the upload arrived on; see httpd_tick() for why that matters.
 *
 * Progress is plain text the page reads with GET /wr?from=, same shape
 * as /log?from=.  Writing a volume reports nothing in between: it runs
 * the board's own env recipe through run_command(), which is the point
 * -- the page and a serial TFTP upgrade have to write a firmware the
 * same way, so there is no hook to report from.  Reading it back
 * afterwards is this file's own loop, so that half does report.
 */
#define WR_LOG_SZ	3072
#define VF_CHUNK	4096		/* one read at a time */
#define VF_STEP		(1UL << 20)	/* ...and this much per pass */

/* Announced by a step that wants another pass; see httpd_flash_step(). */
#define FLASH_MORE	0
#define FLASH_DONE	1
#define FLASH_FAIL	(-1)

static char		wr_log[WR_LOG_SZ];
static int		wr_used;
static int		flash_running;
/*
 * httpd_tick() drives the write, and sits four thousand lines above
 * httpd_flash_step() in a file that carries no forward declarations.  A
 * pointer armed by do_httpd(), where both are in scope, closes that gap.
 */
static int		(*flash_stepper)(void);
static int		flash_in_step;	/* no stepping from inside a step */
static int		flash_result;	/* what the last step returned */
static int		flash_stage;
static int		flash_i;
static int		flash_said;
static ulong		flash_t0;
static u32		flash_wrote;
/*
 * A step is announced ("s ...") one pass ahead of running it, so the page
 * can say what the board is doing before the board stops answering.  A
 * pass is HTTPD_TICK_MS and the page polls every 700 ms, so a long step --
 * the erase of a UBI rebuild above all -- used to start before the page
 * had heard of it, and the page showed the step before for its whole
 * length.  Now the next pass waits until a /wr answer has carried the
 * announcement out, plus a moment for it to cross the wire.  A page that
 * went away holds the write up by WR_HOLD_MS at most.
 */
#define WR_HOLD_MS	1500
#define WR_GRACE_MS	250
static int		wr_hold;	/* wr_used just after the announcement */
static ulong		wr_hold_t0;
static int		wr_sent;	/* how far a /wr answer has reached */
static ulong		wr_sent_t;
static char		wr_buf[2048];
static char		wr_qs[48];
static int		wr_len;
static u8		vf_buf[VF_CHUNK] __aligned(64);
static ulong		vf_off;
static ulong		vf_size;
static ulong		vf_next;
static u32		vf_crc;
static u32		vf_src;

static void wr_reset(void)
{
	wr_used = 0;
	wr_log[0] = '\0';
	wr_hold = 0;
	wr_sent = 0;
}

/*
 * Append only: the page reads from a byte offset it remembers, so what is
 * already out there can never move.  A full buffer drops the line rather
 * than shifting the rest -- at forty-odd short lines per write, that is a
 * margin nothing reaches.
 */
static void wr_printf(const char *fmt, ...)
{
	va_list ap;
	int room = WR_LOG_SZ - wr_used, n;

	if (room <= 1)
		return;

	va_start(ap, fmt);
	/*
	 * vsnprintf() returns what it wanted to write (lib/vsprintf.c's
	 * ADDCH counts past the end), so a cut line is clamped to what
	 * actually landed.  Adding the raw value let wr_used run past
	 * WR_LOG_SZ and httpd_wr() copy from beyond wr_log.
	 */
	n = vsnprintf(wr_log + wr_used, room, fmt, ap);
	va_end(ap);
	wr_used += n < room ? n : room - 1;

	if (fmt[0] == 's' && fmt[1] == ' ') {
		wr_hold = wr_used;
		wr_hold_t0 = get_timer(0);
	}
}

/* Whether the step just announced should wait for the page to hear of it. */
static int wr_holding(void)
{
	if (!wr_hold)
		return 0;
	if ((wr_sent >= wr_hold && get_timer(wr_sent_t) >= WR_GRACE_MS) ||
	    get_timer(wr_hold_t0) >= WR_HOLD_MS) {
		wr_hold = 0;

		return 0;
	}

	return 1;
}

/*
 * Per-unit factory data that the ubi layout keeps as UBI volumes, from
 * CONFIG_HTTPD_FACTORY_VOLS: "name:size name:size ...".  Their exact size is
 * checked before anything is written -- a wrong file must not get near the
 * volume holding the factory MAC.
 */
struct fvol {
	char	name[UBIVOL_NAME_MAX];
	u32	size;
};

static struct fvol	fvols[MAX_FVOLS];
static int		nfvols;

/*
 * Cached once per httpd invocation: env_get_hex() walks the whole environment,
 * and rx() is called for every single segment of a multi-megabyte upload.
 */
static ulong up_base;

/*
 * The head of the request being read, which is what a connection is
 * classified from.  Its own buffer, and that is the point: the classifier
 * used to read the staging area, the same memory an upload lands in.
 * net/tcp.c has one static stream, so up_owner -- a pointer to it -- is
 * equal for every connection there will ever be, and a request arriving
 * while an image sat staged was taken for that upload's own bytes and
 * copied over the head of it.  A /wr poll during a write did that every
 * 700 ms; what saved it was landing on the HTTP and multipart headers
 * that precede the first part, a few hundred bytes short of the image.
 * Nothing could arrive mid-write at all before 0.3.0.
 *
 * Only the first 320 bytes are ever looked at (httpd_classify_get()),
 * and httpd_parse() and st_begin() read their headers from the staging
 * area itself, where their own bytes did land.
 */
static char req_head[HDRBUF_SZ];

/*
 * The upload lands at $loadaddr and is flashed from where it landed, so the
 * ceiling is where U-Boot itself begins -- not the top of DRAM, which is
 * where it relocated its code, malloc arena, FDT and stack to.  The bottom
 * edge of all that is gd->start_addr_sp, and the stack grows down from
 * there, which is what the margin is for.  A factory all_flash.bin is
 * 235.6 MiB and still clears this on a 512 MiB board; a fixed limit would
 * either forbid it outright or let a larger file run into U-Boot.
 */
#define UPLOAD_MARGIN	(2 << 20)

static ulong upload_max(void)
{
	ulong top = gd->start_addr_sp;

	if (up_base + UPLOAD_MARGIN >= top)
		return 0;

	return top - up_base - UPLOAD_MARGIN;
}

/*
 * A download is staged at the top of the same region, growing down, rather
 * than at $loadaddr: httpd_rx() copies every new request to $loadaddr, so a
 * second browser tab would otherwise scribble over the first bytes of a
 * transfer in flight.  The gap below $loadaddr covers that plus the largest
 * read /check does there (a whole static fip volume, about a megabyte); an
 * upload would run through any gap at all, so it is refused outright while a
 * download is in flight rather than budgeted for.
 *
 * Only one window of a download lives there at a time, so the space left
 * over is not what limits a backup any more -- see the /dump section.
 */
#define DUMP_GAP	(2 << 20)

/* Reported by /dumpinfo once a read has finished; see httpd_dumpinfo(). */
static u32	dump_last_seq;
static u32	dump_last_len;
static u32	dump_last_crc;
static int	dump_last_holes;
static char	dump_last_name[128];


static void fvols_parse(void)
{
#ifdef CONFIG_HTTPD_FACTORY_VOLS
	const char *p = CONFIG_HTTPD_FACTORY_VOLS;

	nfvols = 0;
	while (*p && nfvols < MAX_FVOLS) {
		struct fvol *v = &fvols[nfvols];
		int n = 0;

		while (*p == ' ' || *p == ',')
			p++;
		while (*p && *p != ':' && *p != ' ' && *p != ',' &&
		       n < UBIVOL_NAME_MAX - 1)
			v->name[n++] = *p++;
		v->name[n] = '\0';
		v->size = 0;
		if (*p == ':') {
			char *e;

			v->size = hextoul(p + 1, &e);
			p = e;
		}
		while (*p && *p != ' ' && *p != ',')
			p++;
		if (n && v->size)
			nfvols++;
	}
#endif
}

/*
 * Panel feedback.  Recovery is a mode the user enters without a serial
 * console, so the front LEDs are the only way to tell them the board is
 * waiting for an upload rather than dead.
 *
 * Which LEDs chase, and in what order, is the board's to say: its U-Boot
 * device tree lists them by phandle, next to boot-led in the same node,
 *
 *	options {
 *		u-boot {
 *			compatible = "u-boot,config";
 *			httpd-chase-leds = <&led_power &led_wan &led_wan_online>;
 *		};
 *	};
 *
 * so nothing here depends on how an LED is named or what colour it is.
 * There is no guessing for a board that leaves the list out: it gets no
 * chase, a line on the console and a warning on the health check -- and
 * the package build refuses a CONFIG_CMD_HTTPD board whose DTB lacks it.
 *
 * An LED the list names but the board does not have is dropped from the
 * chase instead of being kept as a frame with every LED dark.
 */
#if CONFIG_IS_ENABLED(LED)
#define LED_CHASE_PROP	"httpd-chase-leds"
#define LED_CHASE_MAX	8
#define LED_CHASE_US	120000

static unsigned int led_pos;
static unsigned int led_n;
static int led_declared;	/* entries in the property, found or not */
static struct udevice *led_dev[LED_CHASE_MAX];
static struct cyclic_info led_cyclic;

/* Looking devices up once keeps the cyclic callback cheap. */
static void led_probe(void)
{
	ofnode opts = ofnode_path("/options/u-boot");
	ofnode node;
	int i;

	led_n = 0;
	led_declared = 0;

	if (ofnode_valid(opts)) {
		for (i = 0; ; i++) {
			node = ofnode_parse_phandle(opts, LED_CHASE_PROP, i);
			if (!ofnode_valid(node))
				break;
			if (led_n < LED_CHASE_MAX &&
			    !uclass_get_device_by_ofnode(UCLASS_LED, node,
							 &led_dev[led_n]))
				led_n++;
		}
		led_declared = i;
	}

	if (led_pos >= led_n)
		led_pos = 0;
}

static const char *led_name(struct udevice *dev)
{
	struct led_uc_plat *plat = dev_get_uclass_plat(dev);

	return plat && plat->label ? plat->label : dev->name;
}

static void led_one(unsigned int i, enum led_state_t state)
{
	if (i < led_n)
		led_set_state(led_dev[i], state);
}

static void led_all(enum led_state_t state)
{
	unsigned int i;

	for (i = 0; i < led_n; i++)
		led_one(i, state);
}

/* One frame of the chase.  Called from the tick, so it stays cheap. */
static void httpd_led_paint(void)
{
	unsigned int i;

	if (!led_n)
		return;

	for (i = 0; i < led_n; i++)
		led_one(i, i == led_pos ? LEDST_ON : LEDST_OFF);

	led_pos = (led_pos + 1) % led_n;
}

static void httpd_led_arm(void)
{
	led_probe();
	led_pos = 0;

	if (!led_declared)
		printf("httpd: no %s in /options/u-boot, no LED chase\n",
		       LED_CHASE_PROP);
}

static void httpd_led_off(void)
{
	led_all(LEDST_OFF);
}

/*
 * A volume write is synchronous, so net_loop()'s timeout handler is not
 * running and the chase above would freeze mid-write.  The SPI-NAND layer
 * calls schedule() once per written page, and that is what ticks the cyclic
 * framework -- so the same chase is driven from here instead.
 *
 * The same figure from the first byte to the reboot prompt, deliberately.
 * This used to flash the whole panel at once, which said "the connection
 * is gone, pull the cable if you like, just keep the power on" -- true
 * back when the answer went out before the write.  It is not any more.
 */
static void httpd_chase(struct cyclic_info *c)
{
	(void)c;

	/*
	 * The same frame as the tick paints.  Turning off only led_pos here
	 * left the LED the tick had just lit burning next to the chase: the
	 * tick has already moved led_pos past it by the time a write starts.
	 */
	httpd_led_paint();
}

/*
 * Both are safe to call twice: cyclic_register() unregisters first and
 * cyclic_unregister() checks the node is on the list (common/cyclic.c).
 */
static void httpd_chase_start(void)
{
	/*
	 * Deliberately not stopping the httpd_tick() chase the way the blink
	 * it replaced had to: these two take turns rather than fight, since
	 * both walk the same led_pos and only one of them runs at a time.
	 * The gap this fills is a single step -- a volume write is
	 * synchronous, so nothing else runs for the ten seconds it takes, and
	 * the panel would sit frozen on one LED.  Stopping the tick would
	 * also stop the clock that takes the step after this one.
	 */
	led_probe();
	cyclic_register(&led_cyclic, httpd_chase, LED_CHASE_US, "httpd-flash");
}

static void httpd_chase_stop(void)
{
	cyclic_unregister(&led_cyclic);
}
#else
static inline void httpd_led_paint(void) { }
static inline void httpd_led_arm(void) { }
static inline void httpd_led_off(void) { }
static inline void httpd_chase_start(void) { }
static inline void httpd_chase_stop(void) { }
#endif

/*
 * ---- the tick ------------------------------------------------------
 *
 * net_loop()'s timeout handler, re-arming itself every pass because
 * net_loop() clears the handler before calling it.  Two jobs: walk the
 * panel chase, and -- while a write is under way -- take its next step.
 *
 * Stepping from here rather than from do_httpd() is the whole point.
 * Leaving net_loop() after every step and coming back was the older
 * shape, and net_loop() opens with an unconditional eth_halt() +
 * eth_init(): twenty-odd stop/starts per write, each throwing away
 * whatever the driver had queued.  A timeout handler is how every other
 * protocol in this stack drives a state machine, and NETLOOP_SUCCESS
 * means "this protocol is finished", not "come back for more".  So a
 * write now runs inside the very net_loop() its upload arrived on, and
 * that loop is left exactly once, when the last step is done.
 *
 * flash_in_step guards the one way this could recurse: a step runs the
 * board's own env recipe through run_command(), and an environment
 * somebody edited could put a network command in it, which would re-enter
 * net_loop() and land back here.  The chase still walks; nothing steps
 * twice.
 *
 * Defined outside the LED block on purpose.  It used to be the LED tick,
 * so a build without CONFIG_LED had no timeout handler at all -- and once
 * the write started stepping off that handler, such a build would have
 * taken one step and then served the page forever with a half-written
 * flash.  The panel is the optional half, not the clock.
 */
#define HTTPD_TICK_MS	120

#if IS_ENABLED(CONFIG_CMD_HTTPD_STOCK_RESTORE)
/* Thousands of lines below, next to the rest of /stock. */
static int st_wipe_step(void);
#endif

static void httpd_tick(void)
{
	httpd_led_paint();

	if (flash_pending && flash_stepper && !flash_in_step &&
	    !wr_holding()) {
		int r;

		flash_in_step = 1;
		r = flash_stepper();
		flash_in_step = 0;

		/* Done or failed: hand the verdict to do_httpd() and let it
		 * out of the loop to clean up. */
		if (r != FLASH_MORE) {
			flash_result = r;
			net_set_state(NETLOOP_SUCCESS);
		}
	}

#if IS_ENABLED(CONFIG_CMD_HTTPD_STOCK_RESTORE)
	/* Behind a finished /stock image, the tail is erased from here. */
	if (st_wipe_step())
		return;
#endif

	net_set_timeout_handler(HTTPD_TICK_MS, httpd_tick);
}

static void httpd_tick_start(void)
{
	httpd_led_arm();
	httpd_led_paint();
	net_set_timeout_handler(HTTPD_TICK_MS, httpd_tick);
}

static void httpd_tick_stop(void)
{
	net_set_timeout_handler(0, NULL);
	httpd_led_off();
}

/*
 * Minimal DHCP server.  It keeps three numbers -- how many offers, how
 * many acks, and who asked last -- because "did the address you are using
 * come from me" is a question the recovery page cannot answer otherwise,
 * and the answer changes the advice: a client on its own static address
 * needs a different next step than one that took a lease.
 *
 *
 * Without it the user has to configure a static address before the recovery
 * page is reachable, which is exactly the kind of step that makes a rescue
 * flow fail.  One fixed lease is handed out; there is no pool and no state to
 * keep, which is all a single-client rescue link needs.
 */
#define DHCP_SERVER_PORT	67
#define DHCP_CLIENT_PORT	68
#define DHCP_MAGIC		0x63825363
#define DHCP_DISCOVER		1
#define DHCP_OFFER		2
#define DHCP_REQUEST		3
#define DHCP_DECLINE		4
#define DHCP_ACK		5
#define DHCP_NAK		6
#define DHCP_MIN_LEN		240
#define DHCP_LEASE_SECS		3600
/*
 * What this board's network is for, in one value.
 *
 *   server   hand out addresses; a cable straight from a PC to this board
 *   static   a fixed address on a network somebody else runs
 *   client   ask that network's router for one
 *
 * They used to be two independent things -- "static or DHCP" plus a separate
 * DHCP-server switch -- which is four combinations for three meanings, and
 * one of the four ("ask a router for an address while also being a router")
 * is simply wrong.  Code had to reach in and turn the switch off behind the
 * user's back to keep it from happening, and that decision then could not be
 * saved, so the switch was write-once-zero: after one saved address it could
 * never be turned back on.  One value cannot express the wrong combination
 * at all, so none of that machinery is needed.
 *
 * Server mode fixes the netmask at /24 and the last octet at 1.  That is not
 * a restriction, it is the truth being written down: dhcp_client_ip() has
 * always handed out (net_ip & 0xffffff00) | 100 with itself as the gateway,
 * so any other netmask described a network the leases did not belong to.
 */
#define NET_SERVER	0
#define NET_STATIC	1
#define NET_CLIENT	2

#define ENV_NETMODE		"web_uboot_netmode"
#define ENV_NETIP		"web_uboot_ipaddr"
#define ENV_NETMASK		"web_uboot_netmask"
/*
 * "<ipaddr> <netmask>" as they were before the first unsaved change on a
 * board whose saved mode does not put ipaddr back at boot, "-" for one that
 * was unset.  See netmode_stash().
 */
#define ENV_NETPREV		"web_uboot_netprev"

static int	netmode = NET_SERVER;
/*
 * The running configuration is not the one in flash: applied without the
 * save box, or a lease.  Nothing depends on it except what the page says --
 * "next boot goes back to ..." -- because the two are kept apart by
 * construction rather than by a flag: an unsaved change writes ipaddr and
 * nothing else, and netmode_load() overwrites ipaddr from web_uboot_ipaddr
 * at every boot -- or, where no saved mode does that (none saved, or
 * client), from the copy netmode_stash() took.  So even a saveenv from somewhere else entirely
 * cannot make an unsaved address outlive the power cycle.
 */
static int	net_unsaved;

static const char *netmode_name(int m)
{
	return m == NET_STATIC ? "static" : m == NET_CLIENT ? "client" :
	       "server";
}

/* Anything unrecognised is the default, which is also the safe one. */
static int netmode_parse(const char *s)
{
	if (s && !strcmp(s, "static"))
		return NET_STATIC;
	if (s && !strcmp(s, "client"))
		return NET_CLIENT;

	return NET_SERVER;
}

/*
 * Whether a lease names this board as the gateway.  On by default, which is
 * what it has always done; off leaves a PC that is also on Wi-Fi with its
 * own way out, so it can still read the guide and fetch images while it
 * talks to this page.  Stored only when off, and read per packet so the
 * switch takes effect with the next lease.
 */
#define ENV_DHCPGW		"web_uboot_dhcp_gw"

static int dhcp_gw_on(void)
{
	const char *s = env_get(ENV_DHCPGW);

	return !(s && !strcmp(s, "0"));
}

static u32	dhcp_offers;
static u32	dhcp_acks;
static u8	dhcp_last_mac[6];

struct dhcp_msg {
	u8	op, htype, hlen, hops;
	u32	xid;
	u16	secs, flags;
	u32	ciaddr, yiaddr, siaddr, giaddr;
	u8	chaddr[16];
	u8	sname[64];
	u8	file[128];
	u32	cookie;
	u8	opts[308];
} __packed;

/* The single address handed to the client: x.x.x.100 on our own subnet. */
static struct in_addr dhcp_client_ip(void)
{
	struct in_addr ip;

	ip.s_addr = (net_ip.s_addr & htonl(0xffffff00)) | htonl(100);

	return ip;
}

/*
 * The value of option @want if it is there and exactly @wlen bytes long, else
 * NULL.  Both tests matter on a malformed packet: a truncated option would
 * read the value from past the end of what arrived, and a wrong length means
 * this is not the option it claims to be.
 */
static const u8 *dhcp_opt(const struct dhcp_msg *m, unsigned int len,
			  u8 want, u8 wlen)
{
	unsigned int i, max;

	if (len <= DHCP_MIN_LEN)
		return NULL;

	max = len - DHCP_MIN_LEN;
	if (max > sizeof(m->opts))
		max = sizeof(m->opts);

	for (i = 0; i + 1 < max;) {
		u8 tag = m->opts[i];
		u8 olen = m->opts[i + 1];

		if (tag == 0) {			/* pad */
			i++;
			continue;
		}
		if (tag == 255)			/* end */
			break;
		if (tag == want) {
			if (olen != wlen || i + 2 + olen > max)
				return NULL;

			return &m->opts[i + 2];
		}
		i += 2 + olen;
	}

	return NULL;
}

static int dhcp_msg_type(const struct dhcp_msg *m, unsigned int len)
{
	const u8 *t = dhcp_opt(m, len, 53, 1);

	return t ? *t : 0;
}

/* An address option, or 0 when it is absent or malformed. */
static u32 dhcp_opt_ip(const struct dhcp_msg *m, unsigned int len, u8 tag)
{
	const u8 *v = dhcp_opt(m, len, tag, 4);
	u32 ip = 0;

	if (v)
		memcpy(&ip, v, 4);

	return ip;
}

static u8 *dhcp_put(u8 *o, u8 tag, u8 len, const void *val)
{
	*o++ = tag;
	*o++ = len;
	memcpy(o, val, len);

	return o + len;
}

static void httpd_dhcp_rx(uchar *pkt, unsigned int dport, struct in_addr sip,
			  unsigned int sport, unsigned int len)
{
	static const uchar bcast_mac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	const struct dhcp_msg *req = (const struct dhcp_msg *)pkt;
	struct dhcp_msg *rep;
	struct in_addr yiaddr, bcast;
	u32 mask, lease;
	int type, reply;
	u8 *o;
	int n;

	if (netmode != NET_SERVER)
		return;
	if (dport != DHCP_SERVER_PORT || len < DHCP_MIN_LEN)
		return;
	if (req->op != 1 || req->cookie != htonl(DHCP_MAGIC))
		return;

	yiaddr = dhcp_client_ip();
	type = dhcp_msg_type(req, len);
	if (type == DHCP_DISCOVER) {
		reply = DHCP_OFFER;
	} else if (type == DHCP_REQUEST) {
		/*
		 * RFC 2131 4.3.2.  There is only one address to give, so a
		 * REQUEST for any other one is answered with a NAK -- most
		 * often a PC that held a lease from the system this board
		 * normally runs, asking for it back after the cable came up.
		 * ACKing it with .100 instead left the client to discard the
		 * mismatch and retry until it gave up and started over; a NAK
		 * sends it straight back to DISCOVER.
		 *
		 * A REQUEST naming another server is that client accepting
		 * somebody else's offer, and is none of our business.
		 */
		u32 sid = dhcp_opt_ip(req, len, 54);
		u32 want = dhcp_opt_ip(req, len, 50);

		if (!want)
			want = req->ciaddr;	/* renewing or rebinding */
		if (sid && sid != net_ip.s_addr) {
			printf("httpd: DHCP REQUEST from %pM is for server %pI4, "
			       "not us; ignored\n", req->chaddr, &sid);
			return;
		}
		if (want && want != yiaddr.s_addr) {
			reply = DHCP_NAK;
			printf("httpd: DHCP REQUEST from %pM for %pI4 -> NAK "
			       "(the lease here is %pI4)\n", req->chaddr, &want,
			       &yiaddr);
		} else {
			reply = DHCP_ACK;
		}
	} else if (type == DHCP_DECLINE) {
		/* The client found the address taken; nothing to hand out instead */
		printf("httpd: DHCP DECLINE from %pM: %pI4 is already in use on "
		       "this link\n", req->chaddr, &yiaddr);
		return;
	} else {
		return;
	}

	rep = (struct dhcp_msg *)(net_tx_packet + net_eth_hdr_size() +
				  IP_UDP_HDR_SIZE);
	memset(rep, 0, sizeof(*rep));
	rep->op = 2;
	rep->htype = 1;
	rep->hlen = 6;
	rep->xid = req->xid;
	rep->flags = req->flags;
	if (reply != DHCP_NAK) {
		rep->yiaddr = yiaddr.s_addr;
		rep->siaddr = net_ip.s_addr;
	}
	memcpy(rep->chaddr, req->chaddr, sizeof(rep->chaddr));
	rep->cookie = htonl(DHCP_MAGIC);

	mask = htonl(0xffffff00);
	lease = htonl(DHCP_LEASE_SECS);

	o = rep->opts;
	*o++ = 53;
	*o++ = 1;
	*o++ = (u8)reply;
	o = dhcp_put(o, 54, 4, &net_ip.s_addr);		/* server id */
	if (reply != DHCP_NAK) {
		o = dhcp_put(o, 1, 4, &mask);		/* subnet mask */
		if (dhcp_gw_on())
			o = dhcp_put(o, 3, 4, &net_ip.s_addr);	/* router */
		o = dhcp_put(o, 51, 4, &lease);		/* lease time */
	}
	*o++ = 255;

	n = (int)((u8 *)o - (u8 *)rep);
	if (n < 300)					/* keep BOOTP-sized */
		n = 300;

	bcast.s_addr = 0xffffffff;
	net_send_udp_packet((uchar *)bcast_mac, bcast, DHCP_CLIENT_PORT,
			    DHCP_SERVER_PORT, n);

	/* A NAK handed nothing out, so it is not what the page counts */
	if (reply == DHCP_NAK)
		return;
	if (reply == DHCP_OFFER)
		dhcp_offers++;
	else
		dhcp_acks++;
	memcpy(dhcp_last_mac, req->chaddr, sizeof(dhcp_last_mac));

	printf("httpd: DHCP %s %pI4 -> %pM\n",
	       reply == DHCP_OFFER ? "OFFER" : "ACK", &yiaddr, req->chaddr);
}

/*
 * httpd_finish() searches a whole upload for the multipart boundary, from
 * the on_rcv_nxt_update callback with the net loop stopped.  A memcmp() at
 * every offset of 235 MiB is seconds of nothing else happening, so a needle
 * that long (the boundary is 40-odd bytes) goes through Horspool and skips
 * about its own length per step.  Short needles -- the "\r\n" and
 * "\r\n\r\n" of a request head -- only ever meet a few hundred bytes.
 */
static int mem_find(const char *hay, int hlen, const char *needle, int nlen)
{
	const u8 *h = (const u8 *)hay, *nd = (const u8 *)needle;
	u8 skip[256];
	int i, last;

	if (nlen <= 0 || hlen < nlen)
		return -1;

	if (nlen < 8 || nlen > 255) {
		for (i = 0; i <= hlen - nlen; i++)
			if (h[i] == nd[0] && !memcmp(h + i, nd, nlen))
				return i;
		return -1;
	}

	last = nlen - 1;
	memset(skip, nlen, sizeof(skip));
	for (i = 0; i < last; i++)
		skip[nd[i]] = last - i;

	for (i = 0; i <= hlen - nlen; i += skip[h[i + last]])
		if (h[i + last] == nd[last] && !memcmp(h + i, nd, last))
			return i;

	return -1;
}

/* The bare flash device: the one MTD that is not somebody's partition. */
static struct mtd_info *flash_master(void)
{
	struct mtd_info *mtd;

	mtd_probe_devices();
	mtd_for_each_device(mtd)
		if (!mtd_is_partition(mtd))
			return mtd;

	return NULL;
}

/* One value out of "a=1&b=2"; 0 when the key is not there. */
static int qs_get(const char *qs, const char *key, char *out, int max)
{
	int n = strlen(key);
	const char *p = qs;

	while (*p) {
		const char *amp = strchr(p, '&');
		int len = amp ? amp - p : (int)strlen(p);

		if (len > n + 1 && !strncmp(p, key, n) && p[n] == '=') {
			int v = len - n - 1;

			if (v > max - 1)
				v = max - 1;
			memcpy(out, p + n + 1, v);
			out[v] = '\0';

			return 1;
		}
		if (!amp)
			break;
		p = amp + 1;
	}

	return 0;
}

/*
 * ---- JSON ----------------------------------------------------------------
 *
 * The GET endpoints build their reply in a static buffer each; this is the
 * little writer they share.  Running out of room is not handled at each
 * write -- it is only flagged, and jb_done() turns the flag into an error
 * reply, because half a JSON document is worth less than none.
 */
/*
 * no-store on all of it: each of these is a reading of the board as it is
 * right now, and none carries a validator for a browser to revalidate
 * against.  A cached /ping is the worst of them -- the page would go on
 * being told the board is there, out of its own cache, with the board
 * long gone.
 */
#define JSON_HDR(status)						\
	"HTTP/1.0 " status "\r\n"					\
	"Content-Type: application/json; charset=utf-8\r\n"		\
	"Cache-Control: no-store\r\n"					\
	"Connection: close\r\n"						\
	"\r\n"

/*
 * The same for the answers that are a line of text rather than a document:
 * /wr and /log stream the console while it is being written, and /envreset,
 * /bootonce and /netmode each report how one action went.  Those last three
 * went without no-store until now -- a browser holding on to "ok saved"
 * would show a later attempt succeeding when the request never left it, on
 * the three endpoints where that reads as the flash having been touched.
 *
 * One macro rather than five copies of the same five lines: this field is
 * being added to three of them today precisely because the other two got it
 * and these did not.
 */
#define TEXT_HDR(status)						\
	"HTTP/1.0 " status "\r\n"					\
	"Content-Type: text/plain; charset=utf-8\r\n"			\
	"Cache-Control: no-store\r\n"					\
	"Connection: close\r\n"						\
	"\r\n"

struct jbuf {
	char	*b;
	int	max;
	int	n;
	int	overflow;	/* the buffer filled up; see jb_done() */
};

static void jb_init(struct jbuf *j, char *b, int max)
{
	j->b = b;
	j->max = max;
	j->n = 0;
	j->overflow = 0;
	b[0] = '\0';
}

/*
 * U-Boot's vsnprintf() returns what it would have written had there been
 * room (lib/vsprintf.c), C99 style, so a return that does not fit the space
 * left is the truncation.  j->n is clamped to what actually landed so it
 * never points past the buffer.  An exact fit -- ending on the last usable
 * byte -- counts as an overflow too, which costs only a reply that was
 * already sitting on the very edge of its buffer.
 */
static void jb_printf(struct jbuf *j, const char *fmt, ...)
{
	va_list ap;
	int room, n;

	if (j->overflow)
		return;
	room = j->max - j->n;
	va_start(ap, fmt);
	n = vsnprintf(j->b + j->n, room, fmt, ap);
	va_end(ap);
	j->n += n < room ? n : room - 1;
	if (j->n >= j->max - 1)
		j->overflow = 1;
}

/* A JSON string: quotes and backslashes escaped, control bytes blanked. */
static void jb_str(struct jbuf *j, const char *s)
{
	char *b = j->b;
	int n = j->n, max = j->max;

	if (j->overflow)
		return;
	if (max - n < 3) {
		j->overflow = 1;
		return;
	}
	b[n++] = '"';
	for (; *s && n < max - 3; s++) {
		if (*s == '"' || *s == '\\')
			b[n++] = '\\';
		b[n++] = ((unsigned char)*s < 0x20) ? ' ' : *s;
	}
	/* Stopped on the end of the buffer, not on the end of the string. */
	if (*s)
		j->overflow = 1;
	b[n++] = '"';
	b[n] = '\0';
	j->n = n;
}

/*
 * A cut reply is not a shorter reply, it is invalid JSON: the page's
 * JSON.parse() throws and it shows nothing at all, with no hint as to why.
 * So a buffer that filled up is thrown away and replaced by a short,
 * well-formed 500, which the page already has an error path for.  The
 * console line is what says which buffer needs to grow.
 */
static int jb_done(struct jbuf *j, const char *what)
{
	if (j->overflow) {
		printf("httpd: %s did not fit in its %d byte buffer\n",
		       what, j->max);
		j->n = snprintf(j->b, j->max,
				JSON_HDR("500 Internal Server Error")
				"{\"err\":\"reply too large\"}");
	}

	return j->n;
}

/* A volume by name on an attached device, or NULL. */
static struct ubi_volume *ubi_vol_find(struct ubi_device *ubi, const char *name)
{
	int i;

	for (i = 0; i < ubi->vtbl_slots; i++)
		if (ubi->volumes[i] && !strcmp(ubi->volumes[i]->name, name))
			return ubi->volumes[i];

	return NULL;
}

/*
 * Link and negotiated speed for the switch ports, read straight out of
 * each PHY over MDIO.
 *
 * Everything else about the link is out of reach here and stays that way:
 * U-Boot's eth uclass has no notion of link at all, and gdm1 hangs off the
 * built-in switch rather than a PHY, so there is no phy_device to ask.  The
 * MDIO bus, on the other hand, is registered and probed by the time this
 * runs, so the PHYs answer directly.
 *
 * Raw register reads on purpose: phy_startup() would block for up to
 * CONFIG_PHY_ANEG_TIMEOUT (ten seconds here) waiting for autonegotiation,
 * which is not something an HTTP handler may do.  BMSR is latching, so it
 * is read twice for the current state rather than the sticky one.
 *
 * Addresses are the switch's own GPHYs.  Nothing here names a board: a port
 * that does not answer is simply left out, which is also what happens on a
 * board that brings out fewer of them.
 */
#define PHY_FIRST	0x9
#define PHY_LAST	0xc

/*
 * The switch's own MDIO, found by what it hangs off rather than by being
 * first.  The eth driver binds it to the switch node's "mdio" child; AN7583
 * also describes two SoC buses (mdio-bus@c8, @cc) with nothing on 0x9-0xc,
 * and should their driver ever be built in, "the first MDIO device" is one
 * of those -- port state would read as no link and the cable would never
 * be bounced, with nothing to say why.
 */
static struct udevice *switch_mdio(void)
{
	struct udevice *dev;

	uclass_foreach_dev_probe(UCLASS_MDIO, dev) {
		ofnode sw = ofnode_get_parent(dev_ofnode(dev));

		if (ofnode_device_is_compatible(sw, "airoha,en7523-switch") ||
		    ofnode_device_is_compatible(sw, "airoha,en7581-switch") ||
		    ofnode_device_is_compatible(sw, "airoha,an7583-switch"))
			return dev;
	}

	return NULL;
}

static void info_ports(struct jbuf *jb)
{
	struct udevice *mdio = switch_mdio();
	int a, first = 1;

	if (!mdio)
		return;

	jb_printf(jb, ",\"ports\":[");
	for (a = PHY_FIRST; a <= PHY_LAST; a++) {
		int bmsr, lpa, s1k, link, spd = 0, fd = 0;

		bmsr = dm_mdio_read(mdio, a, MDIO_DEVAD_NONE, MII_BMSR);
		if (bmsr < 0 || bmsr == 0xffff)
			continue;		/* nothing at this address */
		bmsr = dm_mdio_read(mdio, a, MDIO_DEVAD_NONE, MII_BMSR);
		if (bmsr < 0)
			continue;
		link = !!(bmsr & BMSR_LSTATUS);
		if (link) {
			s1k = dm_mdio_read(mdio, a, MDIO_DEVAD_NONE, MII_STAT1000);
			lpa = dm_mdio_read(mdio, a, MDIO_DEVAD_NONE, MII_LPA);
			if (s1k > 0 && (s1k & (LPA_1000FULL | LPA_1000HALF))) {
				spd = 1000;
				fd = !!(s1k & LPA_1000FULL);
			} else if (lpa > 0 && (lpa & LPA_100)) {
				spd = 100;
				fd = !!(lpa & LPA_100FULL);
			} else if (lpa > 0) {
				spd = 10;
				fd = !!(lpa & LPA_10FULL);
			}
		}
		jb_printf(jb, "%s{\"p\":%d,\"link\":%d,\"speed\":%d,\"fd\":%d}",
			  first ? "" : ",", a - PHY_FIRST + 1, link, spd, fd);
		first = 0;
	}
	jb_printf(jb, "]");
}

/*
 * Pull the cable out and put it back, once per boot, the moment the server
 * is ready.
 *
 * With the cable already in, the PC saw link long before anything here was
 * listening -- through BL2, U-Boot and the boot menu -- asked for an address
 * into silence, gave up and took a 169.254 one.  Windows then asks again only
 * every five minutes or so, which is why pulling and replugging the cable was
 * the fix everyone found: a link that goes away and comes back is what makes
 * a PC ask straight away.  So do that for them.
 *
 * Only ports that have link, and only in server mode -- on someone else's
 * network nobody is waiting for us to hand out an address.  Once per boot
 * because net_loop() comes back through httpd_start_server() after every
 * "stay on the page" write and every address change, with the page open and
 * watching; and synchronously, before anything listens, so that nothing can
 * leave the loop with a port still powered down.  The link is gone for this
 * long plus however long autonegotiation takes, a couple of seconds at
 * gigabit.
 */
#define LINK_BOUNCE_MS	1000

static void httpd_link_bounce(void)
{
	static int done;
	int bmcr[PHY_LAST - PHY_FIRST + 1];
	struct udevice *mdio;
	int a, n = 0;

	if (done || netmode != NET_SERVER)
		return;
	done = 1;

	mdio = switch_mdio();
	if (!mdio)
		return;

	for (a = PHY_FIRST; a <= PHY_LAST; a++) {
		int *b = &bmcr[a - PHY_FIRST];
		int bmsr;

		*b = -1;
		dm_mdio_read(mdio, a, MDIO_DEVAD_NONE, MII_BMSR);  /* latched */
		bmsr = dm_mdio_read(mdio, a, MDIO_DEVAD_NONE, MII_BMSR);
		if (bmsr < 0 || bmsr == 0xffff || !(bmsr & BMSR_LSTATUS))
			continue;
		*b = dm_mdio_read(mdio, a, MDIO_DEVAD_NONE, MII_BMCR);
		if (*b < 0 || *b == 0xffff) {
			*b = -1;
			continue;
		}
		dm_mdio_write(mdio, a, MDIO_DEVAD_NONE, MII_BMCR,
			      *b | BMCR_PDOWN);
		n++;
	}
	if (!n)
		return;

	printf("httpd: bouncing link on %d port(s) so the PC asks for an "
	       "address again\n", n);
	mdelay(LINK_BOUNCE_MS);

	for (a = PHY_FIRST; a <= PHY_LAST; a++)
		if (bmcr[a - PHY_FIRST] >= 0)
			dm_mdio_write(mdio, a, MDIO_DEVAD_NONE, MII_BMCR,
				      bmcr[a - PHY_FIRST] & ~BMCR_PDOWN);
}

/*
 * The half of /info that changes while the page is open: the address, what
 * the DHCP server has handed out, and the link state of each port.  Served on
 * its own as /net because that is the half worth asking for again -- /info
 * re-attaches UBI, which rescans every eraseblock and prints a screenful, and
 * "which socket is the cable in" must not cost that.
 */
static void info_net(struct jbuf *jb)
{
	const char *m;

	jb_printf(jb, "\"net\":{\"ip\":\"%pI4\",\"mask\":\"%pI4\","
		  "\"gw\":\"%pI4\",\"server\":\"%pI4\",\"dev\":",
		  &net_ip, &net_netmask, &net_gateway, &net_server_ip);
	jb_str(jb, eth_get_name());
	jb_printf(jb, ",\"mode\":\"%s\",\"ram\":%d,\"dgw\":%d",
		  netmode_name(netmode), net_unsaved, dhcp_gw_on());
	jb_printf(jb, ",\"offer\":%u,\"ack\":%u", dhcp_offers, dhcp_acks);
	if (dhcp_offers || dhcp_acks)
		jb_printf(jb, ",\"client\":\"%pM\"", dhcp_last_mac);
	/*
	 * What the flash holds, which is a different question from what is
	 * running: the page names it in so many words ("next boot goes back
	 * to ...") rather than leaving the user to guess what "not saved"
	 * costs.  Absent means this board has never been through this page,
	 * and the answer is the default environment -- server mode on
	 * whatever ipaddr it ships with.
	 */
	m = env_get(ENV_NETMODE);
	if (m) {
		jb_printf(jb, ",\"saved\":{\"mode\":\"%s\"",
			  netmode_name(netmode_parse(m)));
		m = env_get(ENV_NETIP);
		if (m) {
			jb_printf(jb, ",\"ip\":");
			jb_str(jb, m);
		}
		m = env_get(ENV_NETMASK);
		if (m) {
			jb_printf(jb, ",\"mask\":");
			jb_str(jb, m);
		}
		jb_printf(jb, "}");
	} else {
		jb_printf(jb, ",\"saved\":null");
	}
	jb_printf(jb, "}");
	info_ports(jb);
}

/*
 * ---- GET /info -----------------------------------------------------------
 *
 * Everything the page shows about the board, read when asked rather than
 * baked in at build time.  Attaching UBI here is what the boot script would
 * have done anyway; on a board with no UBI yet the field is simply null.
 *
 * "fip" inside "ubi" says whether the flash holds a U-Boot at all.  A board
 * running a U-Boot that came in over xmodem has none, and the page turns
 * that into a warning before the user reboots into nothing.
 */
static int httpd_info(void)
{
	static char part_name[] = UBI_PART;
	struct mtd_info *master;
	struct jbuf jb;
	const char *s;
	int len, i, first;

#define P(...)	jb_printf(&jb, __VA_ARGS__)
#define S(str)	jb_str(&jb, str)

	jb_init(&jb, info_buf, sizeof(info_buf));
	P(JSON_HDR("200 OK") "{\"web\":\"" WEB_VERSION "\",\"model\":");

	s = fdt_getprop(gd->fdt_blob, 0, "model", NULL);
	S(s ? s : "");

	/* The root compatible list ends with the SoC. */
	P(",\"soc\":");
	s = fdt_getprop(gd->fdt_blob, 0, "compatible", &len);
	if (s && len > 0) {
		const char *p = s, *last = s;

		while (p < s + len && *p) {
			last = p;
			p += strlen(p) + 1;
		}
		S(last);
	} else {
		S("");
	}

	P(",\"ram\":%llu", (unsigned long long)gd->ram_size);

	P(",\"mac\":\"%pM\",", net_ethaddr);
	info_net(&jb);
	P(",\"uboot\":");
	S(version_string);

	master = flash_master();
	if (master) {
		struct mtd_info *part;

		P(",\"flash\":{\"name\":");
		S(master->name);
		P(",\"size\":%llu,\"erase\":%u,\"page\":%u,\"oob\":%u},\"parts\":[",
		  (unsigned long long)master->size, master->erasesize,
		  master->writesize, master->oobsize);
		first = 1;
		list_for_each_entry(part, &master->partitions, node) {
			P("%s{\"n\":", first ? "" : ",");
			S(part->name);
			P(",\"o\":%llu,\"s\":%llu}",
			  (unsigned long long)part->offset,
			  (unsigned long long)part->size);
			first = 0;
		}
		P("]");
	}

	/*
	 * The ceiling httpd_parse() rejects against.  Reporting it lets the
	 * page say no while the file is still on the user's disk, instead of
	 * after a couple of hundred megabytes have gone over the wire.
	 */
	P(",\"uploadmax\":%llu", (unsigned long long)upload_max());
	P(",\"stock\":%d", IS_ENABLED(CONFIG_CMD_HTTPD_STOCK_RESTORE) ? 1 : 0);
	P(",\"log\":%d", IS_ENABLED(CONFIG_CONSOLE_RECORD) ? 1 : 0);

	P(",\"fv\":[");
	for (i = 0; i < nfvols; i++) {
		P("%s{\"n\":", i ? "," : "");
		S(fvols[i].name);
		P(",\"s\":%u}", fvols[i].size);
	}
	P("]");

	if (!ubi_part(part_name, NULL)) {
		struct ubi_device *ubi = ubi_get_device(0);

		if (ubi) {
			/*
			 * avail is what is not handed to any volume.  The page
			 * draws the occupancy bar from the volume sizes and needs
			 * this to tell the two remainders apart: space still to be
			 * given out, and what UBI keeps for itself (the layout
			 * volume and the bad-block reserve).  Subtracting the
			 * volumes from pebs alone lumps them together.
			 */
			P(",\"ubi\":{\"leb\":%d,\"pebs\":%d,\"avail\":%d,"
			  "\"fip\":%d,\"vols\":[",
			  ubi->leb_size, ubi->good_peb_count, ubi->avail_pebs,
			  ubi_vol_find(ubi, "fip") ? 1 : 0);
			first = 1;
			for (i = 0; i < ubi->vtbl_slots; i++) {
				struct ubi_volume *v = ubi->volumes[i];

				if (!v)
					continue;
				P("%s{\"i\":%d,\"n\":", first ? "" : ",",
				  v->vol_id);
				S(v->name);
				P(",\"t\":\"%s\",\"s\":%lld,\"u\":%lld}",
				  v->vol_type == UBI_DYNAMIC_VOLUME ?
				  "dynamic" : "static",
				  (long long)v->reserved_pebs * ubi->leb_size,
				  v->used_bytes);
				first = 0;
			}
			P("]}");
			ubi_put_device(ubi);
		} else {
			P(",\"ubi\":null");
		}
	} else {
		P(",\"ubi\":null");
	}

	P("}");
#undef P
#undef S

	return jb_done(&jb, "/info");
}

static int info_len;

/*
 * ---- GET /net ------------------------------------------------------------
 *
 * What the network tab polls: the same "net" and "ports" objects /info
 * carries, without the device tree, the flash geometry or the UBI attach.
 * Cheap enough to ask for every few seconds, which is what a link light has
 * to be to mean anything.
 */
static char net_buf[1024];
static int net_len;

static int httpd_net(void)
{
	struct jbuf jb;

	jb_init(&jb, net_buf, sizeof(net_buf));
	jb_printf(&jb, JSON_HDR("200 OK") "{");
	info_net(&jb);
	jb_printf(&jb, "}");

	return jb_done(&jb, "/net");
}

/*
 * ---- GET /check ----------------------------------------------------------
 *
 * A health check, run when asked: is there a BL2 where the BootROM looks
 * for one, how many blocks are bad, does the UBI attach, are the volumes
 * the boot needs present and intact.  Factory volumes report whether they
 * can be read; the ri volume also shows the MAC stored in it.  Each item
 * is one row on the page with a green / amber / red mark.
 *
 * Static volumes are read in full: UBI checks their CRC on the way, which
 * is what turns "the volume exists" into "the volume is intact".  The reads
 * land at $loadaddr, which is free whenever no upload is in flight.
 */
#define CHK_OK		0
#define CHK_WARN	1
#define CHK_FAIL	2

/* Both the BL2 image and the FIP are fiptool containers. */
#define FIP_TOC_MAGIC	0xaa640001
#define FIT_MAGIC	0xd00dfeed
/*
 * Where the BootROM expects the BL2 container inside the bl2 partition; the
 * 2 KiB before it are left erased (see web_uboot_write_bl2 in the defenv).
 */
#define BL2_PART	"bl2"
#define BL2_IMAGE_OFF	0x800
/*
 * What ubi_write_fip creates the volume as when there is none yet
 * ("ubi create fip 0x100000 static").  An existing volume is measured
 * instead -- this is only the bound for the create path.
 */
#define FIP_VOL_BYTES	0x100000

static char check_buf[8192];
static int check_len;

/*
 * Sixteen rows is too many to read as one list, so each carries the
 * section it belongs to and the page puts a heading in when it changes.
 */
static const char *chk_g = "";

static void chk_group(const char *g)
{
	chk_g = g;
}

static void chk_item(struct jbuf *jb, int *first, const char *name, int status,
		     const char *fmt, ...)
{
	char v[320];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(v, sizeof(v), fmt, ap);
	va_end(ap);

	jb_printf(jb, "%s{\"n\":", *first ? "" : ",");
	jb_str(jb, name);
	jb_printf(jb, ",\"s\":%d,\"g\":", status);
	jb_str(jb, chk_g);
	jb_printf(jb, ",\"v\":");
	jb_str(jb, v);
	jb_printf(jb, "}");
	*first = 0;

	printf("httpd: check %s/%s: %s%s\n", chk_g, name,
	       status == CHK_OK ? "" : status == CHK_WARN ? "[warn] " : "[FAIL] ",
	       v);
}

static int all_ff(const u8 *p, int n)
{
	while (n--)
		if (*p++ != 0xff)
			return 0;

	return 1;
}

/*
 * The MAC found in the factory volume this run, so the last item can say
 * whether the address U-Boot is actually using came from there.  After a
 * UBI rebuild it will not have, and that is the thing worth knowing.
 */
static u8 chk_fmac[6];
static int chk_fmac_ok;

/* ASCII only: mixing %pM with UTF-8 in one format string garbles the MAC. */
static void fmt_mac(char *dst, int dstsz, const u8 *m)
{
	snprintf(dst, dstsz, "%02x:%02x:%02x:%02x:%02x:%02x",
		 m[0], m[1], m[2], m[3], m[4], m[5]);
}

/*
 * CONFIG_HTTPD_FACTORY_MAC is "volume:offset".  1 if this factory volume
 * is the one that holds the MAC, with *off set to the byte offset.
 */
static int fvol_mac_off(const char *vol, ulong *off)
{
#ifdef CONFIG_HTTPD_FACTORY_MAC
	const char *spec = CONFIG_HTTPD_FACTORY_MAC;
	const char *colon;
	int n;

	if (!spec || !*spec)
		return 0;
	colon = strchr(spec, ':');
	if (!colon)
		return 0;
	n = colon - spec;
	if (n <= 0 || strncmp(spec, vol, n) || vol[n])
		return 0;
	*off = hextoul(colon + 1, NULL);
	return 1;
#else
	return 0;
#endif
}

/*
 * A value out of the environment compiled into this image.  env_get_default()
 * would be the obvious call, but it answers through a 32-byte static buffer,
 * so bootcmd and the menu entries come back truncated -- useless for telling
 * "changed" from "unchanged".  The default environment is a flat list of
 * "name=value" strings ending in an empty one, so read it directly.
 */
static const char *defenv_get(const char *name)
{
	const char *p = (const char *)default_environment;
	int n = strlen(name);

	while (*p) {
		if (!strncmp(p, name, n) && p[n] == '=')
			return p + n + 1;
		p += strlen(p) + 1;
	}

	return NULL;
}

/*
 * The env version, bootcmd and the menu.  A saved environment older than the
 * running U-Boot, or one somebody edited by hand, is a common reason a board
 * stops booting -- and with no serial console there is otherwise no way to
 * see it.
 * Everything is compared against what this build ships, never a hardcoded
 * expectation, so the check keeps telling the truth as the defenv changes.
 */
static void chk_env_defaults(struct jbuf *jb, int *first)
{
	const char *cur = env_get(ENV_VER);
	const char *def = defenv_get(ENV_VER);
	char name[24];
	int n = 0, dn = 0, i;

	if (!def)
		;			/* this build does not version its env */
	else if (!cur)
		chk_item(jb, first, ENV_VER, CHK_WARN,
			 "环境中没有这一项，当前 U-Boot 为 %s。引导菜单为旧版本，下次正常启动时自动刷新",
			 def);
	else if (!strcmp(cur, def))
		chk_item(jb, first, ENV_VER, CHK_OK, "%s，与当前 U-Boot 一致",
			 cur);
	else
		chk_item(jb, first, ENV_VER, CHK_WARN,
			 "闪存中为 %s，当前 U-Boot 为 %s。下次正常启动时自动刷新引导菜单",
			 cur, def);

	cur = env_get("bootcmd");
	def = defenv_get("bootcmd");
	if (!cur)
		chk_item(jb, first, "bootcmd", CHK_FAIL,
			 "未设置，设备不会自行引导。可在「环境变量」页恢复默认值");
	else if (def && !strcmp(cur, def))
		chk_item(jb, first, "bootcmd", CHK_OK, "与当前版本默认值一致");
	else if (def)
		chk_item(jb, first, "bootcmd", CHK_WARN,
			 "与当前版本默认值不同，已被修改：%s", cur);
	else
		chk_item(jb, first, "bootcmd", CHK_OK, "%s", cur);

	for (i = 0; i < 16; i++) {
		snprintf(name, sizeof(name), "bootmenu_%d", i);
		if (env_get(name))
			n++;
		if (defenv_get(name))
			dn++;
	}
	if (!n)
		chk_item(jb, first, "引导菜单", CHK_WARN, "无条目");
	else if (dn && n != dn)
		chk_item(jb, first, "引导菜单", CHK_WARN,
			 "%d 项，当前版本默认为 %d 项", n, dn);
	else
		chk_item(jb, first, "引导菜单", CHK_OK, "%d 项", n);
}

/*
 * How worn the chip is.  UBI counts erases per block, which is the closest
 * thing to a lifetime figure available without vendor-specific commands.
 */
/*
 * Whether there is room left to write a firmware.  A recovery page that
 * says "all fine" and then refuses the upload for want of space has not
 * helped anybody.
 *
 * avail_pebs alone is the wrong number to show, and on a working device it
 * is always the alarming one: OpenWrt's first boot grows rootfs_data over
 * every block fit did not take, so a perfectly healthy router reports 0 MiB
 * free and gets warned it may not fit a firmware -- on a chip with 200 MiB
 * ready for one.
 *
 * What an upload has to play with is that plus what the write frees on its
 * way in.  Both fit writers -- the board's ubi_write_production and the
 * built-in DEF_WRITE_FIT -- remove fit and rootfs_data before creating the
 * new fit, so those two volumes are room, not occupancy.  Count them, and
 * say where the room comes from so the number can be checked against the
 * volume table right above it.
 *
 * The bootloader is deliberately not in that list: a fip write lands in
 * place inside the volume's own reservation and needs nothing freed.
 */
static void chk_avail(struct jbuf *jb, int *first, struct ubi_device *ubi)
{
	static const char * const reclaim[] = { "fit", "rootfs_data" };
	u64 idle = (u64)ubi->avail_pebs * ubi->leb_size;
	int pebs = ubi->avail_pebs;
	char how[200] = "";
	unsigned int i;
	u64 room;

	for (i = 0; i < ARRAY_SIZE(reclaim); i++) {
		struct ubi_volume *v = ubi_vol_find(ubi, reclaim[i]);

		if (v)
			pebs += v->reserved_pebs;
	}

	room = (u64)pebs * ubi->leb_size;
	/*
	 * Result first, then where it comes from: on a working device the
	 * idle figure is 0 and the whole number is reclaimed space, which
	 * only makes sense once the reader has been told what gets deleted.
	 */
	if (room != idle)
		snprintf(how, sizeof(how),
			 "。当前空闲 %llu MiB；写入固件时会先删掉 fit 与 rootfs_data，再腾出 %llu MiB",
			 (unsigned long long)(idle >> 20),
			 (unsigned long long)((room - idle) >> 20));

	chk_item(jb, first, "可写空间",
		 room < (16ULL << 20) ? CHK_WARN : CHK_OK,
		 "刷机可用 %llu MiB（%d 个逻辑擦除块）%s%s",
		 (unsigned long long)(room >> 20), pebs, how,
		 room < (16ULL << 20) ?
		 "。不一定装得下一份固件；可先删掉不用的卷" : "");
}

static void chk_wear(struct jbuf *jb, int *first, struct ubi_device *ubi)
{
	int worn = ubi->max_ec > 20000;

	chk_item(jb, first, "磨损", worn ? CHK_WARN : CHK_OK,
		 "擦写次数最大 %d、平均 %d%s", ubi->max_ec, ubi->mean_ec,
		 worn ? "，接近 SLC 颗粒常见寿命，需留意坏块增长" : "");
}

/* Every block of the chip; the one holding the BL2 is the one that matters. */
/*
 * Whether the partitions actually tile the chip.  Deliberately order
 * independent: compare the sum of the sizes against the span they cover,
 * so a list that is not sorted by offset still gives the right answer.
 */
static void chk_parts(struct jbuf *jb, int *first, struct mtd_info *m)
{
	struct mtd_info *p;
	u64 lo = ~0ULL, hi = 0, sum = 0, span;
	int n = 0;

	list_for_each_entry(p, &m->partitions, node) {
		if (p->offset < lo)
			lo = p->offset;
		if (p->offset + p->size > hi)
			hi = p->offset + p->size;
		sum += p->size;
		n++;
	}
	if (!n) {
		chk_item(jb, first, "分区表", CHK_WARN,
			 "没有分区，只能按 flash 偏移读写");
		return;
	}

	span = hi - lo;
	if (sum > span)
		chk_item(jb, first, "分区表", CHK_FAIL,
			 "%d 个分区，声明的大小合计 %llu 字节，超过它们覆盖的 %llu：有重叠",
			 n, (unsigned long long)sum, (unsigned long long)span);
	else if (sum < span)
		chk_item(jb, first, "分区表", CHK_WARN,
			 "%d 个分区，0x%llx–0x%llx 之间有 %llu 字节没有划进任何分区",
			 n, (unsigned long long)lo, (unsigned long long)hi,
			 (unsigned long long)(span - sum));
	else if (lo || hi != m->size)
		chk_item(jb, first, "分区表", CHK_OK,
			 "%d 个分区，连续覆盖 0x%llx–0x%llx；片上其余 %llu MiB 未划分",
			 n, (unsigned long long)lo, (unsigned long long)hi,
			 (unsigned long long)(m->size - span) >> 20);
	else
		chk_item(jb, first, "分区表", CHK_OK, "%d 个分区，连续覆盖整片", n);
}

static void chk_badblocks(struct jbuf *jb, int *first, struct mtd_info *m)
{
	struct mtd_info *bl2 = get_mtd_device_nm(BL2_PART);
	char list[80];
	int n = 0, nbad = 0, inbl2 = 0;
	loff_t off;

	if (IS_ERR(bl2))
		bl2 = NULL;

	for (off = 0; off < m->size; off += m->erasesize) {
		if (!mtd_block_isbad(m, off))
			continue;
		nbad++;
		if (bl2 && off >= bl2->offset && off < bl2->offset + bl2->size)
			inbl2 = 1;
		if (n < (int)sizeof(list) - 12)
			n += snprintf(list + n, sizeof(list) - n, "%s0x%llx",
				      n ? " " : "", (unsigned long long)off);
	}
	if (bl2)
		put_mtd_device(bl2);

	if (inbl2)
		chk_item(jb, first, "坏块", CHK_FAIL,
			 "%d 个，其中 bl2 分区所在块已损坏，BootROM 可能无法读取 BL2：%s",
			 nbad, list);
	else if (nbad)
		chk_item(jb, first, "坏块", CHK_WARN,
			 "%d 个，已由 UBI 避开：%s%s", nbad, list,
			 n >= (int)sizeof(list) - 12 ? " …" : "");
	else
		chk_item(jb, first, "坏块", CHK_OK, "无");
}

static void chk_bl2(struct jbuf *jb, int *first, u8 *buf)
{
	struct mtd_info *bl2 = get_mtd_device_nm(BL2_PART);
	size_t rl = 0;
	int ret;

	if (IS_ERR(bl2)) {
		chk_item(jb, first, "BL2", CHK_WARN, "没有名为 " BL2_PART " 的分区，无法检查");
		return;
	}

	ret = mtd_read(bl2, 0, BL2_IMAGE_OFF + 64, &rl, buf);
	put_mtd_device(bl2);

	if (ret && ret != -EUCLEAN) {
		chk_item(jb, first, "BL2", CHK_FAIL, "bl2 分区读取失败（%d）", ret);
		return;
	}
	if (get_unaligned_le32(buf + BL2_IMAGE_OFF) == FIP_TOC_MAGIC)
		chk_item(jb, first, "BL2", CHK_OK, "0x%x 处有 BL2 镜像%s",
			 BL2_IMAGE_OFF,
			 ret == -EUCLEAN ? "（本次读取已纠正位翻转）" : "");
	else if (all_ff(buf, BL2_IMAGE_OFF + 64))
		chk_item(jb, first, "BL2", CHK_FAIL,
			 "bl2 分区为空。断电后 BootROM 无法找到 BL2，请在「引导升级」页上传");
	else
		chk_item(jb, first, "BL2", CHK_WARN,
			 "0x%x 处无 BL2 镜像头，可能为原厂或第三方引导程序", BL2_IMAGE_OFF);
}

/*
 * Read a whole static volume; UBI verifies the CRC of every LEB on the way.
 * Returns the number of bytes now at buf, or -1 with the item reported.
 */
static int chk_read_static(struct jbuf *jb, int *first, const char *what,
			   struct ubi_volume *v, u8 *buf, ulong max)
{
	int ret;

	if (v->used_bytes <= 0 || (ulong)v->used_bytes > max) {
		chk_item(jb, first, what, CHK_FAIL, "卷内 %lld 字节，无法读取",
			 v->used_bytes);
		return -1;
	}
	ret = ubi_volume_read(v->name, (char *)buf, 0, v->used_bytes);
	if (ret) {
		chk_item(jb, first, what, CHK_FAIL,
			 "读取失败（%d），卷内数据校验未通过，内容已损坏", ret);
		return -1;
	}

	return v->used_bytes;
}

static void chk_fip(struct jbuf *jb, int *first, struct ubi_device *ubi,
		    u8 *buf, ulong max)
{
	struct ubi_volume *v = ubi_vol_find(ubi, "fip");
	int n;

	if (!v) {
		chk_item(jb, first, "fip 卷", CHK_FAIL,
			 "不存在。闪存中无 U-Boot，断电后无法启动，请在「引导升级」页上传 U-Boot 文件");
		return;
	}
	n = chk_read_static(jb, first, "fip 卷", v, buf, max);
	if (n < 0)
		return;
	if (get_unaligned_le32(buf) == FIP_TOC_MAGIC)
		chk_item(jb, first, "fip 卷", CHK_OK, "%d 字节，校验通过", n);
	else
		chk_item(jb, first, "fip 卷", CHK_FAIL,
			 "卷内不是 FIP（头 0x%08x），请重新上传 U-Boot 文件",
			 get_unaligned_le32(buf));
}

static void chk_fit(struct jbuf *jb, int *first, struct ubi_device *ubi,
		    u8 *buf)
{
	struct ubi_volume *v = ubi_vol_find(ubi, "fit");
	u32 tot;
	int ret;

	if (!v) {
		chk_item(jb, first, "fit 卷", CHK_FAIL,
			 "不存在。闪存中无固件，请在「日常刷机」页上传");
		return;
	}
	ret = ubi_volume_read(v->name, (char *)buf, 0, 4096);
	if (ret) {
		chk_item(jb, first, "fit 卷", CHK_FAIL, "读取失败（%d）", ret);
		return;
	}
	if (get_unaligned_be32(buf) != FIT_MAGIC) {
		chk_item(jb, first, "fit 卷", CHK_FAIL,
			 "卷内不是 FIT 镜像（头 0x%08x），请重新上传固件",
			 get_unaligned_be32(buf));
		return;
	}
	tot = fdt_totalsize(buf);
	if (tot > v->used_bytes)
		chk_item(jb, first, "fit 卷", CHK_FAIL,
			 "镜像声明 %u 字节，卷内仅 %lld 字节。固件不完整，请重新上传",
			 tot, v->used_bytes);
	else
		chk_item(jb, first, "fit 卷", CHK_OK, "FIT 镜像，%u 字节", tot);
}

/* Unix time to "YYYY-MM-DD HH:MM"; no RTC and no libc to lean on. */
static void fmt_date(char *dst, int max, u32 t)
{
	static const u8 dim[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	u32 days = t / 86400, secs = t % 86400;
	int y = 1970, m, leap;

	for (;;) {
		leap = !(y % 4) && (y % 100 || !(y % 400));
		if (days < (u32)(leap ? 366 : 365))
			break;
		days -= leap ? 366 : 365;
		y++;
	}
	for (m = 0; m < 11; m++) {
		u32 dm = dim[m] + (m == 1 && leap);

		if (days < dm)
			break;
		days -= dm;
	}

	snprintf(dst, max, "%04d-%02d-%02d %02d:%02d", y, m + 1, (int)days + 1,
		 (int)(secs / 3600), (int)(secs % 3600 / 60));
}

/*
 * Make the head of the FIT readable by libfdt without reading all of it.
 *
 * A sysupgrade FIT is megabytes because the image data sits inside the
 * struct block, which puts the strings block right at the end -- and libfdt
 * needs both ends to resolve a property name.  So read the head and the
 * strings block separately, drop them next to each other, and rewrite the
 * header to describe where they actually landed.  Root properties are the
 * first thing in the struct block, so the head is always enough for them.
 *
 * Anything unexpected returns -1 and the caller just reports the size it
 * already knows; this is a nicety, not a verdict on the image.
 */
#define FIT_HEAD_MAX	(128 << 10)
#define FIT_STRS_MAX	(64 << 10)

static int fit_stitch(struct ubi_volume *v, u8 *buf, ulong max)
{
	u32 tot, stroff, strsz, structoff, head;

	if (ubi_volume_read(v->name, (char *)buf, 0, 64))
		return -1;
	if (get_unaligned_be32(buf) != FIT_MAGIC)
		return -1;

	tot	  = fdt_totalsize(buf);
	stroff	  = fdt_off_dt_strings(buf);
	strsz	  = fdt_size_dt_strings(buf);
	structoff = fdt_off_dt_struct(buf);

	if (!strsz || strsz > FIT_STRS_MAX || structoff >= stroff ||
	    (u64)stroff + strsz > tot || (u64)tot > (u64)v->used_bytes)
		return -1;

	head = stroff < FIT_HEAD_MAX ? stroff : FIT_HEAD_MAX;
	if ((u64)head + strsz > max)
		return -1;

	if (ubi_volume_read(v->name, (char *)buf, 0, head))
		return -1;
	if (ubi_volume_read(v->name, (char *)buf + head, stroff, strsz))
		return -1;

	fdt_set_off_dt_strings(buf, head);
	fdt_set_size_dt_strings(buf, strsz);
	fdt_set_size_dt_struct(buf, head - structoff);
	fdt_set_totalsize(buf, head + strsz);

	return fdt_check_header(buf) ? -1 : 0;
}

/*
 * Which build is actually on the flash.  "the fit volume holds a FIT" is
 * what chk_fit() answers; this answers "which one", which is the line
 * somebody asking for help in a forum thread actually needs to paste.
 */
static void chk_firmware(struct jbuf *jb, int *first, struct ubi_device *ubi,
			 u8 *buf, ulong max)
{
	struct ubi_volume *v = ubi_vol_find(ubi, "fit");
	const char *desc, *compat = NULL;
	const fdt32_t *ts;
	char when[24];
	int node, len;

	if (!v)
		return;			/* chk_fit already said so */

	if (fit_stitch(v, buf, max)) {
		chk_item(jb, first, "固件", CHK_WARN,
			 "无法读取镜像描述，仅能确认为 FIT 格式");
		return;
	}

	desc = fdt_getprop(buf, 0, "description", NULL);
	ts = fdt_getprop(buf, 0, "timestamp", &len);
	when[0] = '\0';
	if (ts && len == 4)
		fmt_date(when, sizeof(when), fdt32_to_cpu(*ts));

	/* The default configuration names the board the image was built for. */
	node = fdt_subnode_offset(buf, 0, "configurations");
	if (node >= 0) {
		const char *dflt = fdt_getprop(buf, node, "default", NULL);
		int cfg = dflt ? fdt_subnode_offset(buf, node, dflt) : -1;

		if (cfg >= 0)
			compat = fdt_getprop(buf, cfg, "compatible", NULL);
	}

	if (!desc && !when[0] && !compat) {
		chk_item(jb, first, "固件", CHK_WARN, "镜像中无描述信息");
		return;
	}

	chk_item(jb, first, "固件", CHK_OK, "%s%s%s%s%s",
		 desc ? desc : "",
		 desc && when[0] ? "，" : "", when,
		 (desc || when[0]) && compat ? "，" : "", compat ? compat : "");
}

/*
 * The environment volumes.  Both start with U-Boot's own CRC of the
 * environment they hold, so four bytes out of each is enough to say whether
 * the two copies are the same environment -- no need to read 124 KiB twice.
 * Returns 1 and *crc when the volume could be read.
 */
static int chk_envvol(struct jbuf *jb, int *first, struct ubi_device *ubi,
		      const char *name, u32 *crc, const char *other,
		      u32 othercrc)
{
	char vol[UBIVOL_NAME_MAX], what[UBIVOL_NAME_MAX + 8];
	u8 b[4];

	snprintf(what, sizeof(what), "%s 卷", name);
	if (!ubi_vol_find(ubi, name)) {
		chk_item(jb, first, what, CHK_WARN,
			 "不存在。U-Boot 环境无处保存，首次正常启动时自动创建");
		return 0;
	}

	strlcpy(vol, name, sizeof(vol));
	if (ubi_volume_read(vol, (char *)b, 0, sizeof(b))) {
		chk_item(jb, first, what, CHK_WARN, "存在，无法读取内容");
		return 0;
	}
	*crc = get_unaligned_le32(b);

	if (!other)
		chk_item(jb, first, what, CHK_OK, "存在，CRC 0x%08x", *crc);
	else if (*crc == othercrc)
		chk_item(jb, first, what, CHK_OK, "存在，与 %s 一致", other);
	else
		chk_item(jb, first, what, CHK_WARN,
			 "存在，CRC 0x%08x，与 %s 的 0x%08x 不同。两份环境内容不一致，启动时使用较新的一份",
			 *crc, other, othercrc);

	return 1;
}

static void chk_fvol(struct jbuf *jb, int *first, struct ubi_device *ubi,
		     struct fvol *f, u8 *buf, ulong max)
{
	struct ubi_volume *v = ubi_vol_find(ubi, f->name);
	char what[UBIVOL_NAME_MAX + 8];
	char mac[18];
	ulong macoff;
	int n;

	snprintf(what, sizeof(what), "%s 卷", f->name);
	if (!v) {
		chk_item(jb, first, what, CHK_WARN, "无法读取，卷不存在");
		return;
	}
	n = chk_read_static(jb, first, what, v, buf, max);
	if (n < 0)
		return;
	if (all_ff(buf, n)) {
		chk_item(jb, first, what, CHK_WARN, "已读取，内容为空");
		return;
	}
	if (fvol_mac_off(f->name, &macoff) && macoff + 6 <= (ulong)n) {
		const u8 *m = buf + macoff;

		if (is_valid_ethaddr(m) && !is_broadcast_ethaddr(m)) {
			memcpy(chk_fmac, m, sizeof(chk_fmac));
			chk_fmac_ok = 1;
			fmt_mac(mac, sizeof(mac), m);
			chk_item(jb, first, what, CHK_OK, "已读取，MAC %s", mac);
		} else {
			chk_item(jb, first, what, CHK_WARN,
				 "已读取，无有效 MAC");
		}
		return;
	}
	chk_item(jb, first, what, CHK_OK, "已读取");
}

static void chk_mac(struct jbuf *jb, int *first)
{
	char mac[18];
	u8 addr[6];

	if (!env_get("ethaddr")) {
		fmt_mac(mac, sizeof(mac), net_ethaddr);
		chk_item(jb, first, "U-Boot MAC", CHK_WARN,
			 "环境中无 ethaddr，本次启动使用随机地址 %s", mac);
	} else if (!eth_env_get_enetaddr("ethaddr", addr)) {
		chk_item(jb, first, "U-Boot MAC", CHK_FAIL,
			 "ethaddr 不是有效地址");
	} else if (chk_fmac_ok && !memcmp(addr, chk_fmac, sizeof(chk_fmac))) {
		fmt_mac(mac, sizeof(mac), addr);
		chk_item(jb, first, "U-Boot MAC", CHK_OK, "%s，与出厂数据一致", mac);
	} else if (chk_fmac_ok) {
		char fac[18];

		fmt_mac(mac, sizeof(mac), addr);
		fmt_mac(fac, sizeof(fac), chk_fmac);
		chk_item(jb, first, "U-Boot MAC", CHK_WARN,
			 "%s，与出厂数据中的 %s 不同。重建 UBI 后常见，将备份的出厂卷写回即可",
			 mac, fac);
	} else {
		fmt_mac(mac, sizeof(mac), addr);
		chk_item(jb, first, "U-Boot MAC", CHK_OK, "%s", mac);
	}
}

/*
 * What the panel will do while this page is up, so a board brought up
 * without the chase list -- or with a phandle to an LED the tree does not
 * have -- shows it here instead of only on the serial console.
 */
static void chk_leds(struct jbuf *jb, int *first)
{
#if CONFIG_IS_ENABLED(LED)
	char v[240];
	int len = 0;
	unsigned int i;

	led_probe();
	if (!led_declared) {
		chk_item(jb, first, "流水灯", CHK_WARN,
			 "U-Boot 设备树未在 /options/u-boot 声明 %s，救砖时指示灯不流水",
			 LED_CHASE_PROP);
		return;
	}
	if (!led_n) {
		chk_item(jb, first, "流水灯", CHK_WARN,
			 "%s 声明了 %d 个灯，一个也没有找到", LED_CHASE_PROP,
			 led_declared);
		return;
	}

	v[0] = '\0';
	for (i = 0; i < led_n && len < (int)sizeof(v); i++)
		len += snprintf(v + len, sizeof(v) - len, "%s%s",
				i ? "、" : "", led_name(led_dev[i]));

	if ((int)led_n < led_declared)
		chk_item(jb, first, "流水灯", CHK_WARN,
			 "%u 个：%s（声明了 %d 个，有 %d 个没有找到）", led_n, v,
			 led_declared, led_declared - (int)led_n);
	else
		chk_item(jb, first, "流水灯", CHK_OK, "%u 个：%s", led_n, v);
#else
	chk_item(jb, first, "流水灯", CHK_WARN, "未启用 CONFIG_LED，救砖时指示灯不流水");
#endif
}

static int httpd_check(void)
{
	static char part_name[] = UBI_PART;
	struct mtd_info *master;
	struct jbuf jb;
	u8 *buf = (u8 *)up_base;
	ulong max = upload_max();
	u32 __maybe_unused e1 = 0, __maybe_unused e2 = 0;
	int __maybe_unused e1ok = 0;
	int first = 1, i;

	jb_init(&jb, check_buf, sizeof(check_buf));

	/*
	 * $loadaddr is the staging area of an upload in flight; and a write
	 * in progress means half of what this would read is on its way in.
	 * Both were unreachable before -- nothing was served between the
	 * answer and the reset -- and both are reachable now.
	 */
	if (up_active || flash_running) {
		jb_printf(&jb, JSON_HDR("503 Service Unavailable") "{\"busy\":1}");
		return jb_done(&jb, "/check");
	}

	chk_fmac_ok = 0;
	printf("httpd: running the health check\n");
	jb_printf(&jb, JSON_HDR("200 OK") "{\"items\":[");

	chk_group("闪存");
	master = flash_master();
	if (!master) {
		chk_item(&jb, &first, "闪存", CHK_FAIL, "没有找到闪存设备");
		goto out;
	}
	chk_item(&jb, &first, "闪存", CHK_OK,
		 "%s，%llu MiB，擦除块 %u KiB，页 %u B，OOB %u B",
		 master->name, (unsigned long long)master->size >> 20,
		 master->erasesize >> 10, master->writesize, master->oobsize);
	chk_parts(&jb, &first, master);

	chk_badblocks(&jb, &first, master);

	chk_group("引导");
	chk_bl2(&jb, &first, buf);
	chk_env_defaults(&jb, &first);

	chk_group("UBI");
	if (ubi_part(part_name, NULL)) {
		chk_item(&jb, &first, "UBI", CHK_FAIL,
			 "无法挂载，闪存上无可用的 UBI。首次迁移请在「引导升级」页启用「重建 UBI」，并同时上传 BL2、U-Boot 与固件");
	} else {
		struct ubi_device *ubi = ubi_get_device(0);

		if (!ubi) {
			chk_item(&jb, &first, "UBI", CHK_FAIL, "已挂载但无法访问");
			goto out;
		}
		chk_item(&jb, &first, "UBI", CHK_OK,
			 "%d 个卷，坏块 %d 个，空闲 %d 个逻辑擦除块",
			 ubi->vol_count, ubi->bad_peb_count, ubi->avail_pebs);

		chk_avail(&jb, &first, ubi);
		chk_wear(&jb, &first, ubi);
		chk_fip(&jb, &first, ubi, buf, max);
		chk_fit(&jb, &first, ubi, buf);
		chk_firmware(&jb, &first, ubi, buf, max);

		chk_group("环境");
#ifdef CONFIG_ENV_UBI_VOLUME
		e1ok = chk_envvol(&jb, &first, ubi, CONFIG_ENV_UBI_VOLUME,
				  &e1, NULL, 0);
#endif
#ifdef CONFIG_ENV_UBI_VOLUME_REDUND
		chk_envvol(&jb, &first, ubi, CONFIG_ENV_UBI_VOLUME_REDUND, &e2,
			   e1ok ? CONFIG_ENV_UBI_VOLUME : NULL, e1);
#endif

		chk_group("出厂数据");
		for (i = 0; i < nfvols; i++)
			chk_fvol(&jb, &first, ubi, &fvols[i], buf, max);

		ubi_put_device(ubi);
	}

	chk_group("出厂数据");
	chk_mac(&jb, &first);

	chk_group("指示灯");
	chk_leds(&jb, &first);
out:
	jb_printf(&jb, "]}");

	return jb_done(&jb, "/check");
}

/*
 * ---- GET /scan?off= ------------------------------------------------------
 *
 * A read-only pass over the chip.  /check answers "will this thing boot";
 * this answers "is the flash still healthy", and only reading every page
 * can tell you that.  The BBT lists the blocks the factory or a failed
 * erase marked, but the pages the ECC is quietly correcting on every read
 * are the ones that say the part is on its way out, and nothing but a read
 * surfaces those.
 *
 * Served in slices, because nothing can be answered while mtd_read() runs.
 * One request covers SCAN_SLICE and reports where to resume; between
 * slices the heartbeat gets its turn and the page moves a progress bar.
 *
 * No state is kept here.  The page adds the numbers up, so an abandoned
 * scan leaves nothing behind that a later one would have to reset -- the
 * mistake /dump made once and had to grow a timeout to undo.
 */
#define SCAN_SLICE	(4 << 20)
#define SCAN_LIST	8

/* Query strings for the endpoints that take one. */
static char	scan_qs[32];
static char	log_qs[32];
static char	scan_buf[1024];
static u8	scan_page[4096];
static int	scan_len;

static int httpd_scan(void)
{
	struct mtd_info *m = flash_master();
	u64 badl[SCAN_LIST], faill[SCAN_LIST];
	struct jbuf jb;
	char o[24];
	u64 off = 0, blk, p;
	u32 step, done = 0;
	int nb = 0, nf = 0, ecc = 0, i;

	jb_init(&jb, scan_buf, sizeof(scan_buf));

	if (!m) {
		jb_printf(&jb, JSON_HDR("500 Internal Server Error")
			  "{\"err\":\"没有找到闪存设备\"}");
		return jb_done(&jb, "/scan");
	}
	if (up_active || dump_busy || flash_running) {
		jb_printf(&jb, JSON_HDR("503 Service Unavailable")
			  "{\"err\":\"设备正忙，等这次传输完成后再扫描\"}");
		return jb_done(&jb, "/scan");
	}

	if (qs_get(scan_qs, "off", o, sizeof(o)))
		off = hextoul(o, NULL);
	off &= ~((u64)m->erasesize - 1);
	if (off > m->size)
		off = m->size;

	/* One page per read, so an ECC correction is counted where it happened. */
	step = (m->writesize && m->writesize <= sizeof(scan_page)) ?
		m->writesize : sizeof(scan_page);

	while (off < m->size && done < SCAN_SLICE) {
		blk = off;
		if (mtd_block_isbad(m, blk)) {
			if (nb < SCAN_LIST)
				badl[nb] = blk;
			nb++;
		} else {
			for (p = blk; p < blk + m->erasesize; p += step) {
				size_t rl = 0;
				int ret = mtd_read(m, p, step, &rl, scan_page);

				if (ret == -EUCLEAN) {
					ecc++;
				} else if (ret) {
					if (nf < SCAN_LIST)
						faill[nf] = p;
					nf++;
					break;
				}
			}
		}
		off = blk + m->erasesize;
		done += m->erasesize;
	}

	jb_printf(&jb, JSON_HDR("200 OK")
		  "{\"off\":%llu,\"size\":%llu,\"blk\":%u,\"done\":%d"
		  ",\"bad\":%d,\"ecc\":%d,\"fail\":%d,\"badlist\":[",
		  (unsigned long long)off, (unsigned long long)m->size,
		  m->erasesize, off >= m->size ? 1 : 0, nb, ecc, nf);
	for (i = 0; i < nb && i < SCAN_LIST; i++)
		jb_printf(&jb, "%s%llu", i ? "," : "",
			  (unsigned long long)badl[i]);
	jb_printf(&jb, "],\"faillist\":[");
	for (i = 0; i < nf && i < SCAN_LIST; i++)
		jb_printf(&jb, "%s%llu", i ? "," : "",
			  (unsigned long long)faill[i]);
	jb_printf(&jb, "]}");

	return jb_done(&jb, "/scan");
}

/*
 * ---- GET /log ------------------------------------------------------------
 *
 * The console output since power-on, from U-Boot's own recording buffer
 * (CONFIG_CONSOLE_RECORD).  For the user without a serial adapter this is
 * the only way to see which flash chip was found, which blocks were
 * skipped, and why a write failed -- the lines this file prints while
 * flashing go there too.  The buffer is read through a copy of its control
 * block, so nothing is consumed.
 *
 * ?from=N serves only what follows byte N, so the page can follow the
 * console the way tail -f does instead of re-fetching 64 KiB every couple
 * of seconds.  That is sound because the record is append-only: U-Boot
 * stops recording when the buffer fills (GD_FLG_RECORD_OVF) rather than
 * wrapping, so a byte at offset N stays the byte at offset N.
 *
 * The "buffer is full" notice is deliberately NOT part of that stream --
 * it sits at the end of the body, so a follower would be handed it again
 * on every poll.  /ping carries the flag instead, and the page says it
 * once, next to the log rather than inside it.
 */
#if IS_ENABLED(CONFIG_CONSOLE_RECORD)
static char log_buf[CONFIG_CONSOLE_RECORD_OUT_SIZE + 512];
static int log_len;

static int httpd_log(void)
{
	static const char hdr[] = TEXT_HDR("200 OK");
	static const char full[] =
		"\n[log buffer full; later output not recorded]\n";
	struct membuf copy = *(struct membuf *)&gd->console_out;
	char from[24], pre[16];
	unsigned long skip = 0;
	char *data;
	int n, len, i, body, tail, k, back, follow;

	follow = qs_get(log_qs, "from", from, sizeof(from));
	if (follow)
		skip = dectoul(from, NULL);

	n = snprintf(log_buf, sizeof(log_buf), "%s", hdr);
	body = n;
	for (i = 0; i < 2; i++) {
		len = membuf_getraw(&copy, sizeof(log_buf) - n - sizeof(full),
				    true, &data);
		if (len <= 0)
			break;
		memcpy(log_buf + n, data, len);
		n += len;
	}

	/*
	 * Following: hand back only what is new, after one line saying how far
	 * that reaches.  The page cannot work the offset out on its own -- it
	 * holds decoded text, not bytes -- so the device has to say it.
	 *
	 * The notice below belongs to the whole-log view; a follower would be
	 * handed it again on every poll, so /ping carries that flag instead.
	 */
	if (follow) {
		if (skip > (unsigned long)(n - body))
			skip = n - body;
		tail = n - body - (int)skip;

		/*
		 * Never end inside a UTF-8 sequence: each response is decoded on
		 * its own, so a split one leaves a replacement character behind
		 * for good.  Walk back to the last lead byte and drop it if what
		 * follows is short; the next poll picks it up whole.
		 */
		for (k = tail, back = 0; k > 0 && back < 4; k--, back++) {
			unsigned char c = log_buf[body + skip + k - 1];
			int need;

			if ((c & 0xc0) == 0x80)
				continue;	/* continuation, keep walking */
			need = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 :
				(c & 0xf0) == 0xe0 ? 3 : 4;
			if (back + 1 < need)
				tail = k - 1;
			break;
		}

		k = snprintf(pre, sizeof(pre), "%lu\n",
			     skip + (unsigned long)tail);
		memmove(log_buf + body + k, log_buf + body + skip, tail);
		memcpy(log_buf + body, pre, k);

		return body + k + tail;
	}

	if (gd->flags & GD_FLG_RECORD_OVF) {
		memcpy(log_buf + n, full, sizeof(full) - 1);
		n += sizeof(full) - 1;
	}

	return n;
}
#else
static const char log_buf[] =
	"HTTP/1.0 404 Not Found\r\n"
	"Content-Type: text/plain; charset=utf-8\r\n"
	"Connection: close\r\n"
	"\r\n"
	"this U-Boot was built without CONFIG_CONSOLE_RECORD\n";
static int log_len;

static int httpd_log(void)
{
	return strlen(log_buf);
}
#endif

/*
 * ---- GET /wr?from= -------------------------------------------------------
 *
 * How far the write has got.  Same shape as /log?from=: first line is the
 * new offset, then the bytes after the one asked for.  The board answers
 * nothing at all while a volume write runs, so the page polls and simply
 * misses a few -- every line stands on its own, and the offset it is told
 * is the offset of what it actually got, never more.
 */
static int httpd_wr(void)
{
	static const char hdr[] = TEXT_HDR("200 OK");
	char from[24];
	int skip = 0;
	int n, room;

	if (qs_get(wr_qs, "from", from, sizeof(from)))
		skip = (int)dectoul(from, NULL);
	if (skip < 0 || skip > wr_used)
		skip = wr_used;

	room = wr_used - skip;
	/* Header, the offset line and a NUL all have to fit as well. */
	if (room > (int)sizeof(wr_buf) - (int)sizeof(hdr) - 16) {
		int cap = (int)sizeof(wr_buf) - (int)sizeof(hdr) - 16;

		/*
		 * Cut after a whole line: the page decodes each answer on
		 * its own, so a split in the middle of a Chinese step would
		 * come out as two U+FFFD for good.  The rest follows on the
		 * next poll from the offset given here.
		 */
		room = cap;
		while (room > 0 && wr_log[skip + room - 1] != '\n')
			room--;
		if (!room)
			room = cap;
	}

	n = snprintf(wr_buf, sizeof(wr_buf), "%s%d\n", hdr, skip + room);
	if (skip + room > wr_sent) {
		wr_sent = skip + room;
		wr_sent_t = get_timer(0);
	}
	if (room > 0) {
		memcpy(wr_buf + n, wr_log + skip, room);
		n += room;
	}
	wr_buf[n] = '\0';

	return n;
}

/*
 * ---- GET /ping -----------------------------------------------------------
 *
 * Two numbers of work, polled every few seconds: is the server answering,
 * and how long has this U-Boot been up.  Uptime rather than a token because
 * it answers a second question for free -- when it goes backwards the board
 * rebooted underneath the page, and the page reloads itself instead of
 * showing volume tables from before the reboot.
 */
/*
 * Sized off the header rather than off the body: JSON_HDR alone is 110 bytes
 * and jb_done()'s own "reply too large" answer is 155, so anything under that
 * cannot report its own overflow either.  At 96 -- what this was -- every
 * single poll was cut mid-"Connection:", header incomplete, body absent.
 * A bare vsnprintf() truncates quietly and nothing checked what it
 * returned, so nothing on the console said otherwise; from the page the
 * board simply never came up as connected.  Built through jb_printf() for the same reason: /ping was
 * the one JSON endpoint writing straight into its buffer, which is exactly
 * how it stayed silently truncated across four releases.
 */
static char ping_buf[256];
static int ping_len;

static int httpd_ping(void)
{
	struct jbuf jb;

	jb_init(&jb, ping_buf, sizeof(ping_buf));

	/*
	 * ovf rides along here because the page polls this anyway, and because
	 * a follower on /log?from= would otherwise never learn that the record
	 * stopped growing for a reason.
	 */
	jb_printf(&jb, JSON_HDR("200 OK") "{\"up\":%lu,\"ovf\":%d}",
		  get_timer(0),
		  IS_ENABLED(CONFIG_CONSOLE_RECORD) &&
		  (gd->flags & GD_FLG_RECORD_OVF) ? 1 : 0);

	return jb_done(&jb, "/ping");
}

/*
 * ---- GET /env ------------------------------------------------------------
 *
 * The environment, read-only.  A board that will not boot usually has a
 * bootcmd or a bootmenu entry somebody edited by hand, and without a serial
 * console there is no way to see that today.  Sorted by name: the hash
 * table's own order means nothing to a reader.
 *
 * Editing is deliberately not offered.  One wrong bootcmd from a page whose
 * whole purpose is rescuing a board that will not boot is a bad trade; the
 * one write offered is the wholesale restore below, which cannot leave the
 * environment in a state this build has never seen.
 */
#define MAX_ENVS	256

static char env_buf[16384];
static int env_len;

/*
 * hwalk_r() hands every live entry to a callback and takes no context
 * pointer, so the list it fills is a static.  That is fine here: the whole
 * server is single threaded and the list is consumed before returning.
 *
 * The table itself cannot be walked from outside: struct env_entry_node is
 * only defined in lib/hashtable.c.
 */
static struct env_entry *env_list[MAX_ENVS];
static int env_n;

static int env_collect(struct env_entry *e)
{
	if (env_n < MAX_ENVS)
		env_list[env_n++] = e;

	return 0;			/* non-zero would stop the walk */
}

static int httpd_env(void)
{
	struct env_entry **list = env_list;
	struct jbuf jb;
	int n, cut = 0, i, j;

	env_n = 0;
	hwalk_r(&env_htab, env_collect);
	n = env_n;

	/* Selection sort: a hundred names at most, and only when asked. */
	for (i = 0; i < n - 1; i++) {
		int m = i;

		for (j = i + 1; j < n; j++)
			if (strcmp(list[j]->key, list[m]->key) < 0)
				m = j;
		if (m != i) {
			struct env_entry *t = list[i];

			list[i] = list[m];
			list[m] = t;
		}
	}

	jb_init(&jb, env_buf, sizeof(env_buf));
	jb_printf(&jb, JSON_HDR("200 OK") "{\"env\":[");
	for (i = 0; i < n; i++) {
		const char *k = list[i]->key;
		const char *v = list[i]->data ? list[i]->data : "";

		/*
		 * Stop on a whole entry rather than let jb_str cut one in
		 * half: a truncated string would make the whole reply
		 * unparseable, and then the page shows nothing at all.
		 */
		if (jb.n + (int)strlen(k) + 2 * (int)strlen(v) + 32 > jb.max) {
			cut = 1;
			break;
		}
		jb_printf(&jb, "%s{\"k\":", i ? "," : "");
		jb_str(&jb, k);
		jb_printf(&jb, ",\"v\":");
		jb_str(&jb, v);
		jb_printf(&jb, "}");
	}
	jb_printf(&jb, "],\"cut\":%d}", cut);

	if (cut)
		printf("httpd: /env listed %d of %d variables\n", i, n);

	return jb_done(&jb, "/env");
}

/*
 * ---- GET /envreset -------------------------------------------------------
 *
 * "env default -a && saveenv".  The recovery case is a saved environment
 * older than this build, or one somebody edited into a state that will not
 * boot -- both of which the built-in fallbacks in this file work around for
 * flashing, but neither of which they fix for the next normal boot.
 *
 * ethaddr is put back by hand.  "env default -a" drops it with everything
 * else; the factory MAC lives in the ri volume and the boot script restores
 * it, but not until the next boot, and the page promises the MAC is not
 * touched.  Keeping the running value is what makes that true.
 */
static char envreset_buf[256];
static int envreset_len;

static int httpd_envreset(void)
{
	struct jbuf jb;
	char mac[20];
	const char *cur;
	int saved;

	cur = env_get("ethaddr");
	strlcpy(mac, cur ? cur : "", sizeof(mac));

	printf("httpd: restoring the default environment\n");
	run_command("env default -a", 0);
	if (mac[0] && !env_get("ethaddr"))
		env_set("ethaddr", mac);

	saved = !run_command("saveenv", 0);
	printf("httpd: default environment %s\n",
	       saved ? "restored and saved" :
		       "restored in RAM only -- saveenv failed");

	/*
	 * These three text/plain answers go through jb_printf() for one
	 * reason: jb_done()'s truncation check is the only one available.
	 * A bare vsnprintf() truncates without a word, so a reply that
	 * outgrew its buffer used to go out cut in half with nothing said
	 * anywhere -- which is exactly how
	 * /ping spent four releases answering with half a header.  All three
	 * fit today with room to spare; the point is that the day one of them
	 * stops fitting, it says so instead of being quietly wrong.
	 *
	 * An overflow replaces the whole buffer, header included, with a
	 * JSON 500, so nothing is left claiming text/plain over a JSON body;
	 * every caller of these three checks the status code before it looks
	 * at the body at all.
	 */
	jb_init(&jb, envreset_buf, sizeof(envreset_buf));
	jb_printf(&jb, TEXT_HDR("200 OK") "%s", saved ? "ok saved" : "ok");

	return jb_done(&jb, "/envreset");
}

/*
 * ---- GET /reboot ---------------------------------------------------------
 *
 * Answer first, reset once the answer has been acknowledged -- same shape as
 * the flash-after-net_loop path, and for the same reason: resetting from
 * inside a tx callback would drop the response on the floor and the page
 * would report a lost connection instead of a reboot.
 */
static const char resp_reboot[] = TEXT_HDR("200 OK") "ok";

static int reboot_pending;
static int boot_pending;

/*
 * ---- GET /boot, GET /bootonce --------------------------------------------
 *
 * /boot runs the normal boot command now, without a power cycle.  If the
 * system does not come up the caller re-enters httpd, so this costs
 * nothing to try.
 *
 * /bootonce arranges for the next boot to stop here instead.  It is one
 * shot and undoes itself: web_uboot_boot_once is cleared and the old bootcmd
 * put back the next time the httpd command is entered at all, so a user who
 * sets it and then walks away still gets a normal boot the time after.
 *
 * The replacement bootcmd keeps the original after it rather than
 * replacing it outright:
 *
 *	httpd ; <original>
 *
 * so even if the restore never happens -- flash gone read-only, power cut
 * at the wrong moment -- the device still boots the system once the httpd
 * session ends.  A one-shot that can strand the board is not worth having.
 */
#define BOOT_CMD	"boot_ubi"
#define ENV_ONCE	"web_uboot_boot_once"
#define ENV_SAVED	"web_uboot_bootcmd_saved"

static char bootonce_buf[256];
static int bootonce_len;

static int httpd_bootonce(void)
{
	const char *cur = env_get("bootcmd");
	char next[CONFIG_SYS_CBSIZE];
	struct jbuf jb;
	const char *msg;

	if (env_get(ENV_ONCE)) {
		msg = "already armed";
		goto out;
	}
	if (!cur || !cur[0]) {
		msg = "no bootcmd to put back afterwards";
		goto out;
	}
	if (strlen(cur) + 8 >= sizeof(next)) {
		msg = "bootcmd is too long to wrap";
		goto out;
	}

	snprintf(next, sizeof(next), "httpd ; %s", cur);
	if (env_set(ENV_SAVED, cur) || env_set("bootcmd", next) ||
	    env_set(ENV_ONCE, "1")) {
		msg = "could not set the environment";
		goto out;
	}
	/* saveenv rather than env_save(): the same path the rest of this
	 * file already uses for saving. */
	msg = run_command("saveenv", 0) ? "armed, but saving failed"
					: "armed and saved";

out:
	jb_init(&jb, bootonce_buf, sizeof(bootonce_buf));
	jb_printf(&jb, TEXT_HDR("200 OK") "%s\n", msg);
	bootonce_len = jb_done(&jb, "/bootonce");

	return bootonce_len;
}

/*
 * ---- GET /wipecfg --------------------------------------------------------
 *
 * Clear OpenWrt's settings: remove rootfs_data, see ENV_REMOVE_ROOTFS.  Done
 * in the request, like /envreset; removing a volume erases what it held, a
 * few seconds on a full-size overlay, which the page's heartbeat rides out.
 * Checked afterwards rather than trusted: a recipe somebody edited can
 * return success without having removed anything.
 */
static char wipecfg_buf[160];
static int wipecfg_len;

static int rootfs_data_there(void)
{
	struct ubi_device *ubi = ubi_get_device(0);
	int there;

	if (!ubi)
		return -1;
	there = !!ubi_vol_find(ubi, "rootfs_data");
	ubi_put_device(ubi);

	return there;
}

static int httpd_wipecfg(void)
{
	static char part_name[] = UBI_PART;
	struct jbuf jb;
	const char *msg;
	int there;

	if (ubi_part(part_name, NULL)) {
		msg = "no UBI to clear";
		goto out;
	}
	there = rootfs_data_there();
	if (there < 0) {
		msg = "UBI attached but not accessible";
		goto out;
	}
	if (!there) {
		msg = "ok, there was no rootfs_data";
		goto out;
	}

	printf("httpd: removing rootfs_data; OpenWrt starts fresh on the next "
	       "boot\n");
	run_command(env_get(ENV_REMOVE_ROOTFS) ? "run " ENV_REMOVE_ROOTFS :
		    DEF_REMOVE_ROOTFS, 0);
	msg = rootfs_data_there() ? "rootfs_data is still there" :
				    "ok removed";

out:
	printf("httpd: /wipecfg: %s\n", msg);
	jb_init(&jb, wipecfg_buf, sizeof(wipecfg_buf));
	jb_printf(&jb, TEXT_HDR("200 OK") "%s\n", msg);
	wipecfg_len = jb_done(&jb, "/wipecfg");

	return wipecfg_len;
}

/*
 * ---- GET /dhcpgw?on=0|1 --------------------------------------------------
 *
 * The gateway switch, saved on the spot: it changes nothing about the
 * address, so it does not go through /netmode and the page stays where it
 * is.  Leases already handed out keep what they got until the cable is
 * replugged.
 */
static char dhcpgw_qs[16];
static char dhcpgw_buf[160];
static int dhcpgw_len;

static int httpd_dhcpgw(void)
{
	struct jbuf jb;
	const char *msg;
	char v[4];
	int on = 1;

	if (qs_get(dhcpgw_qs, "on", v, sizeof(v)))
		on = v[0] != '0';

	if (env_set(ENV_DHCPGW, on ? NULL : "0"))
		msg = "could not set the environment";
	else
		msg = run_command("saveenv", 0) ? "ok, but saving failed" :
						  "ok saved";
	printf("httpd: DHCP gateway %s: %s\n", on ? "on" : "off", msg);

	jb_init(&jb, dhcpgw_buf, sizeof(dhcpgw_buf));
	jb_printf(&jb, TEXT_HDR("200 OK") "%s\n", msg);
	dhcpgw_len = jb_done(&jb, "/dhcpgw");

	return dhcpgw_len;
}

/*
 * ---- GET /netmode?mode=&ip=&mask=&save= ----------------------------------
 *
 * The whole network configuration in one request: which of the three modes,
 * the address it needs (none, in client mode), and whether it outlives the
 * power cycle.  It replaces /netset, /netdhcp and /netdhcpd, which between
 * them could express a combination that was never meant to exist.
 *
 * Deferred, like the two it replaces: the answer has to leave on the old
 * address, because the browser is holding a connection to it and a reply
 * sourced from the new one would simply be dropped.  So the request only
 * records what to do, the netloop ends, and the change happens in httpd()
 * before the server is started again.
 *
 * Not saved unless asked -- and here "not saved" is exact rather than
 * approximate.  An unsaved change writes ipaddr and netmask, which are
 * runtime state, and leaves web_uboot_netmode / _ipaddr / _netmask alone.
 * Those three are the only thing netmode_load() reads at the next boot, so
 * nothing that runs saveenv for its own reasons -- /bootonce, /envreset --
 * can turn a deliberately temporary address into a permanent one on the way
 * past.  A wrong address is the one mistake this page cannot talk its way
 * out of, and pulling the power has to keep working as the way back.
 *
 * Client mode has a problem no code here can fix: after it succeeds the
 * device is at an address the browser does not know.  The lease comes from
 * the upstream router, so that is where the user has to look -- by MAC.  The
 * page says so before it starts.  It is offered anyway because putting the
 * device on an existing network is sometimes the only way in, and it falls
 * back to the previous address when no lease arrives.
 */
/*
 * 512 where the other two text/plain answers get 256: netmode_reply() takes a
 * 160 byte message, and TEXT_HDR is 104 -- 264 in the worst case, which 256
 * does not hold.  It held before only because these answers carried no
 * Cache-Control, and that is the kind of arithmetic nobody redoes when adding
 * a header field.
 */
static char netmode_buf[512];
static int netmode_len;
static int netmode_pending;
/*
 * Parsed and held, but not yet acted on.  netmode_pending is raised only
 * once the answer has been acknowledged.  Raising it while the request was
 * still being parsed left it set for an answer that may never have arrived,
 * and the next exit from net_loop() -- an acknowledged flash, say -- would
 * be taken for the address change: the address would move, do_httpd() would
 * loop, httpd_start_server() would clear flash_pending, and the write the
 * page had already been told was accepted would be dropped without a word.
 */
static int netmode_armed;
static int nm_mode;
static char nm_ip[20];
static char nm_mask[20];
static int nm_save;
static char netmode_qs[96];

/* Dotted quad, and not one that cannot be an interface address. */
static int ip_ok(const char *s, struct in_addr *out)
{
	unsigned long v[4];
	const char *p = s;
	u32 a;
	int i;

	for (i = 0; i < 4; i++) {
		char *e;

		v[i] = simple_strtoul(p, &e, 10);
		/*
		 * simple_strtoul() wraps silently on overflow, so a long
		 * enough run of digits comes back as any value at all --
		 * "4294967296" as 0.  An octet is at most three digits, so
		 * the length is capped before the value is even looked at.
		 */
		if (e == p || e - p > 3 || v[i] > 255)
			return 0;
		if (i < 3 && *e != '.')
			return 0;
		if (i == 3 && *e)
			return 0;
		p = e + 1;
	}
	a = (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3];
	if (!a || a == 0xffffffff || v[0] == 0 || v[0] == 127 || v[0] >= 224)
		return 0;
	if (out)
		out->s_addr = htonl(a);

	return 1;
}

/*
 * A netmask, which ip_ok() cannot judge -- it turns down 255.255.255.0 for
 * the leading 255 and waves through 10.0.0.1, which is not a mask at all.
 * What makes a mask is a run of ones followed by a run of zeros, so ~m + 1
 * is the lowest set bit of m and a mask is exactly the value for which that
 * bit falls outside ~m.  All zeros and all ones are excluded separately:
 * neither leaves a subnet the device could be addressed on.
 */
static int mask_ok(const char *s)
{
	unsigned long v[4];
	const char *p = s;
	u32 m;
	int i;

	for (i = 0; i < 4; i++) {
		char *e;

		v[i] = simple_strtoul(p, &e, 10);
		if (e == p || e - p > 3 || v[i] > 255)
			return 0;
		if (i < 3 && *e != '.')
			return 0;
		if (i == 3 && *e)
			return 0;
		p = e + 1;
	}
	m = (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3];
	if (!m || m == 0xffffffff)
		return 0;

	return ((~m + 1) & ~m) == 0;
}

static int netmode_reply(const char *fmt, ...)
{
	char msg[160];
	struct jbuf jb;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	jb_init(&jb, netmode_buf, sizeof(netmode_buf));
	jb_printf(&jb, TEXT_HDR("200 OK") "%s\n", msg);
	netmode_len = jb_done(&jb, "/netmode");

	return netmode_len;
}

/*
 * Parsed now, applied once the answer is out.
 *
 * Server mode does not take the address as given: the last octet becomes 1
 * and the netmask becomes /24, because dhcp_client_ip() hands out
 * (net_ip & 0xffffff00) | 100 with this board as the gateway.  Letting the
 * user pick either of those would only let them describe a network their own
 * leases do not belong to.  The page shows the two addresses it is about to
 * produce while the field is still being typed in, so this is not a surprise
 * sprung after the button.
 */
static int httpd_netmode(void)
{
	char m[12], ip[20], mask[20], sv[8];
	struct in_addr a;
	int mode;

	netmode_armed = 0;

	if (!qs_get(netmode_qs, "mode", m, sizeof(m)))
		return netmode_reply("bad mode");
	if (!strcmp(m, "server"))
		mode = NET_SERVER;
	else if (!strcmp(m, "static"))
		mode = NET_STATIC;
	else if (!strcmp(m, "client"))
		mode = NET_CLIENT;
	else
		return netmode_reply("bad mode");

	nm_ip[0] = '\0';
	nm_mask[0] = '\0';

	if (mode != NET_CLIENT) {
		if (!qs_get(netmode_qs, "ip", ip, sizeof(ip)) || !ip_ok(ip, &a))
			return netmode_reply("bad ip");

		if (mode == NET_SERVER) {
			a.s_addr = (a.s_addr & htonl(0xffffff00)) | htonl(1);
			snprintf(nm_ip, sizeof(nm_ip), "%pI4", &a);
			strlcpy(nm_mask, "255.255.255.0", sizeof(nm_mask));
		} else {
			strlcpy(nm_ip, ip, sizeof(nm_ip));
			if (qs_get(netmode_qs, "mask", mask, sizeof(mask))) {
				if (!mask_ok(mask))
					return netmode_reply("bad mask");
				strlcpy(nm_mask, mask, sizeof(nm_mask));
			} else {
				strlcpy(nm_mask, "255.255.255.0",
					sizeof(nm_mask));
			}
		}
	}

	nm_mode = mode;
	nm_save = qs_get(netmode_qs, "save", sv, sizeof(sv)) && sv[0] == '1';
	netmode_armed = 1;

	return netmode_reply("ok %s %s %s %s", netmode_name(mode),
			     nm_ip[0] ? nm_ip : "-",
			     nm_mask[0] ? nm_mask : "-",
			     nm_save ? "saved" : "ram");
}

/* Ask the network's own router for an address, and keep the old one if it
 * does not answer. */
static void netmode_lease(void)
{
	char back[20], bootfile[256];
	int bf_stash;
	const char *cur;

	cur = env_get("ipaddr");
	strlcpy(back, cur ? cur : "", sizeof(back));
	cur = env_get("bootfile");
	bf_stash = !cur || strlen(cur) < sizeof(bootfile);
	if (bf_stash)
		strlcpy(bootfile, cur ? cur : "", sizeof(bootfile));

	/*
	 * An empty bootfile keeps net_loop(DHCP) from going on to tftp.  A
	 * name too long to stash is left alone: a tftp timeout is cheaper
	 * than writing a truncated name back afterwards.
	 */
	if (bf_stash)
		env_set("bootfile", NULL);
	printf("httpd: asking the upstream router for an address\n");
	if (net_loop(DHCP) < 0) {
		printf("httpd: no lease; staying at %s\n", back);
		if (back[0])
			env_set("ipaddr", back);
	} else {
		printf("httpd: now at %pI4 -- look for %pM on the router\n",
		       &net_ip, net_ethaddr);
	}

	/*
	 * Put it back whichever way that went: emptying bootfile is a local
	 * trick to stop net_loop(DHCP) short of a tftp transfer, not
	 * something the user asked for, and leaving it empty would break the
	 * next tftpboot for no visible reason.
	 */
	if (bf_stash)
		env_set("bootfile", bootfile[0] ? bootfile : NULL);
}

/*
 * Whether netmode_load() puts ipaddr back from a saved value by itself:
 * server and static do, from web_uboot_ipaddr.  No saved mode leaves ipaddr
 * alone, and client saves the decision but never an address.
 */
static int netmode_restores_ip(void)
{
	const char *m = env_get(ENV_NETMODE);

	return m && netmode_parse(m) != NET_CLIENT && env_get(ENV_NETIP);
}

/*
 * Where netmode_load() has nothing to put back, an unsaved address would
 * simply stay in ipaddr -- and the next saveenv from /dhcpgw, /bootonce or
 * anything else would make it permanent.  In client mode that is also the
 * address a boot without a lease falls back to.  Keep what was there before
 * the first unsaved change instead.  RAM only, like the change itself: if
 * nothing saves, the flash still holds the old ipaddr anyway; if something
 * does, this goes along with it.
 */
static void netmode_stash(void)
{
	const char *ip = env_get("ipaddr"), *mask = env_get("netmask");
	char v[64];

	if (netmode_restores_ip() || env_get(ENV_NETPREV))
		return;

	snprintf(v, sizeof(v), "%s %s", ip ? ip : "-", mask ? mask : "-");
	env_set(ENV_NETPREV, v);
}

/*
 * The other half, at boot.  Dropped from RAM once used, so the next saveenv
 * clears it from flash too and a later ipaddr set by hand is left alone.
 */
static void netmode_unstash(void)
{
	const char *s = env_get(ENV_NETPREV);
	char v[64], *mask;

	if (!s)
		return;

	strlcpy(v, s, sizeof(v));
	env_set(ENV_NETPREV, NULL);

	mask = strchr(v, ' ');
	if (!mask)
		return;
	*mask++ = '\0';

	env_set("ipaddr", strcmp(v, "-") ? v : NULL);
	env_set("netmask", strcmp(mask, "-") ? mask : NULL);
}

/*
 * Saving client mode saves no address, but the saveenv that goes with it
 * writes ipaddr as it stands -- which may be a temporary one from an unsaved
 * change, and the next boot without a lease would fall back to it.  It stays
 * for this boot: the page is on it, and promises that a failed lease leaves
 * it there.  What goes to flash alongside is the address to come back to,
 * for netmode_load() to put back as it does after any unsaved change: the
 * stash already taken, or else what the old saved mode would have restored.
 * Read off that old mode, so before it is overwritten.
 */
static void netmode_client_stash(void)
{
	const char *ip, *mask;
	char v[64];

	if (env_get(ENV_NETPREV) || !netmode_restores_ip())
		return;

	ip = env_get(ENV_NETIP);
	if (netmode_parse(env_get(ENV_NETMODE)) == NET_SERVER)
		mask = "255.255.255.0";
	else if (!(mask = env_get(ENV_NETMASK)))
		mask = env_get("netmask");
	snprintf(v, sizeof(v), "%s %s", ip, mask ? mask : "-");
	env_set(ENV_NETPREV, v);
}

/* Apply what /netmode asked for, once the answer is out. */
static void netmode_apply(void)
{
	if (!netmode_pending)
		return;
	netmode_pending = 0;

	/* Before ipaddr is touched: it is the old value being kept. */
	if (!nm_save)
		netmode_stash();

	netmode = nm_mode;
	printf("httpd: %s mode%s%s, %s\n", netmode_name(netmode),
	       nm_ip[0] ? " at " : "", nm_ip,
	       nm_save ? "saved" : "this boot only");

	if (nm_mode != NET_CLIENT) {
		env_set("ipaddr", nm_ip);
		env_set("netmask", nm_mask);
	}

	if (!nm_save) {
		net_unsaved = 1;
	} else {
		if (nm_mode == NET_CLIENT)
			netmode_client_stash();
		env_set(ENV_NETMODE, netmode_name(nm_mode));
		/*
		 * A lease is not an address this board owns, so client mode
		 * saves the decision and nothing else: the next boot asks for
		 * one again rather than coming up on somebody else's address.
		 */
		env_set(ENV_NETIP, nm_ip[0] ? nm_ip : NULL);
		env_set(ENV_NETMASK, nm_mask[0] ? nm_mask : NULL);
		/*
		 * A saved mode is what netmode_load() goes by from now on --
		 * client mode by the stash as well, for the address it falls
		 * back to.
		 */
		if (nm_mode != NET_CLIENT)
			env_set(ENV_NETPREV, NULL);
		if (run_command("saveenv", 0)) {
			printf("httpd: saving failed; this lasts until "
			       "reboot\n");
			net_unsaved = 1;
		} else {
			net_unsaved = 0;
		}
	}

	if (nm_mode == NET_CLIENT)
		netmode_lease();
}

/*
 * Put the saved configuration into effect.  net_init_loop() reads ipaddr on
 * every entry to net_loop(), so this has to have written it before the first
 * one.
 *
 * A board with no web_uboot_netmode has never been through this page.  It
 * gets server mode -- which is what this whole thing is for, a cable
 * straight from a PC -- and its ipaddr is left exactly as it is: that value
 * may well have been set by hand on the serial console, and there is no
 * reason for this page to have an opinion about it before it is used.  The
 * one exception is an unsaved change that some other saveenv carried into
 * flash, which netmode_unstash() undoes -- here and in client mode alike.
 */
static void netmode_load(void)
{
	const char *s = env_get(ENV_NETMODE);

	/*
	 * Before the client-mode lease, which falls back to whatever ipaddr
	 * holds when nobody answers.
	 */
	if (netmode_restores_ip())
		env_set(ENV_NETPREV, NULL);
	else
		netmode_unstash();

	if (!s)
		return;

	netmode = netmode_parse(s);

	s = env_get(ENV_NETIP);
	if (s && netmode != NET_CLIENT)
		env_set("ipaddr", s);

	if (netmode == NET_SERVER) {
		env_set("netmask", "255.255.255.0");
	} else if (netmode == NET_STATIC) {
		s = env_get(ENV_NETMASK);
		if (s)
			env_set("netmask", s);
	}
}

/*
 * Once per boot rather than once per httpd invocation: the boot script runs
 * "while true ; do httpd ; sleep 1 ; done", and re-applying on the way back
 * in would undo an unsaved change every time the server restarted, and ask
 * for a fresh lease every time round the loop.
 */
static void netmode_boot(void)
{
	static int done;

	if (done)
		return;
	done = 1;

	netmode_load();
	if (netmode == NET_CLIENT)
		netmode_lease();
}

/*
 * Undo /bootonce.  Called once from do_httpd(), before the loop -- not from
 * httpd_start_server(), which net_loop(HTTPD) runs on every entry: a user
 * who arms /bootonce and then changes the address or does a "stay up" flash
 * would have had it disarmed under them on the way back in.
 */
static void bootonce_disarm(void)
{
	const char *saved = env_get(ENV_SAVED);

	if (!env_get(ENV_ONCE))
		return;

	printf("httpd: this boot was the one-shot recovery entry; "
	       "putting bootcmd back\n");
	if (saved && saved[0])
		env_set("bootcmd", saved);
	env_set(ENV_SAVED, NULL);
	env_set(ENV_ONCE, NULL);
	if (run_command("saveenv", 0))
		printf("httpd: could not save; it will arm again next boot\n");
}

/*
 * ---- upload --------------------------------------------------------------
 */

/* Answer 400 with the reason; the page shows it under the progress bar. */
static void httpd_reject(const char *fmt, ...)
{
	va_list ap;
	int n;

	n = snprintf(resp_reject, sizeof(resp_reject),
		     "HTTP/1.0 400 Bad Request\r\n"
		     "Content-Type: text/plain; charset=utf-8\r\n"
		     "Connection: close\r\n"
		     "\r\n");
	va_start(ap, fmt);
	vsnprintf(resp_reject + n, sizeof(resp_reject) - n, fmt, ap);
	va_end(ap);

	printf("httpd: rejecting upload: %s\n", resp_reject + n);
	up_failed = 1;
}

/*
 * Parse the request headers, which sit at the very beginning of the stream
 * (already stored at $loadaddr).  Only called once enough contiguous bytes
 * have arrived.  The individual parts are located later, in one pass over the
 * complete body.
 */
static void httpd_parse(u32 rx_bytes)
{
	char hdr[HDRBUF_SZ];
	const char *base = (const char *)up_base;
	char *p, *q;
	int n, hdr_end, blen;
	ulong clen;

	n = rx_bytes < HDRBUF_SZ - 1 ? rx_bytes : HDRBUF_SZ - 1;
	memcpy(hdr, base, n);
	hdr[n] = '\0';

	hdr_end = mem_find(hdr, n, "\r\n\r\n", 4);
	if (hdr_end < 0) {
		/*
		 * Only the first HDRBUF_SZ - 1 bytes are ever searched, so
		 * once that much has arrived with no blank line in it the end
		 * of the headers is never going to be found.  Saying so beats
		 * returning "not yet" forever, which left the browser waiting
		 * on an answer that could not come and the staging area held
		 * until the stream timed out.
		 */
		if (n >= HDRBUF_SZ - 1)
			httpd_reject("request headers are longer than the %d "
				     "bytes this server reads", HDRBUF_SZ - 1);

		return;			/* headers not complete yet */
	}
	hdr_end += 4;

	p = strstr(hdr, "Content-Length:");
	if (!p) {
		httpd_reject("missing Content-Length");
		return;
	}
	p += 15;
	/*
	 * Every client sends "Content-Length: N" with a space after the colon,
	 * and U-Boot's simple_strtoul() -- unlike the C library one -- does not
	 * skip leading whitespace (lib/strto.c only handles a 0x prefix).  It
	 * would return 0 here and the upload would be rejected as empty.
	 */
	while (*p == ' ' || *p == '\t')
		p++;
	clen = simple_strtoul(p, NULL, 10);
	/* Headers sit in the same region, so they count against the room. */
	if (!clen || (u64)hdr_end + (u64)clen > (u64)upload_max()) {
		httpd_reject("upload is %lu bytes, at most %lu fit in RAM",
			     clen, upload_max());
		return;
	}

	p = strstr(hdr, "boundary=");
	if (!p) {
		httpd_reject("not a multipart upload");
		return;
	}
	p += 9;
	if (*p == '"') {
		p++;
		for (q = p; *q && *q != '"'; q++)
			;
	} else {
		for (q = p; *q && *q != '\r' && *q != '\n' && *q != ';'; q++)
			;
	}
	blen = q - p;
	if (blen <= 0 || blen > (int)sizeof(up_bound) - 5) {
		httpd_reject("bad multipart boundary");
		return;
	}
	/* Each part ends right before CRLF + "--" + boundary. */
	memcpy(up_bound, "\r\n--", 4);
	memcpy(up_bound + 4, p, blen);
	up_bound_len = blen + 4;

	up_body = hdr_end;
	up_total = hdr_end + clen;
	up_parsed = 1;

	printf("httpd: expecting %lu body bytes at +%u\n", clen, up_body);
}

static struct up_part *part_find(const char *name)
{
	int i;

	for (i = 0; i < up_nparts; i++)
		if (!strcmp(up_parts[i].name, name))
			return &up_parts[i];

	return NULL;
}

/* A short text field, as a C string; NULL when absent or too long. */
static const char *part_text(const char *name, char *buf, int max)
{
	struct up_part *p = part_find(name);

	if (!p || p->size >= (u32)max)
		return NULL;
	memcpy(buf, (void *)p->addr, p->size);
	buf[p->size] = '\0';

	return buf;
}

/*
 * The name goes straight into a run_command() string, so anything outside
 * this set -- a space, a semicolon -- would be parsed as more command.  UBI
 * itself allows more than this, but nothing the recovery page needs to write
 * uses characters beyond it.
 */
static int vol_name_ok(const char *s)
{
	int n = strlen(s);

	if (!n || n >= UBIVOL_NAME_MAX)
		return 0;

	for (; *s; s++) {
		char c = *s;

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
			return 0;
	}

	return 1;
}

static struct fvol *fvol_find(const char *field)
{
	int i;

	if (strncmp(field, FIELD_FVOL_PREFIX, strlen(FIELD_FVOL_PREFIX)))
		return NULL;
	field += strlen(FIELD_FVOL_PREFIX);
	for (i = 0; i < nfvols; i++)
		if (!strcmp(fvols[i].name, field))
			return &fvols[i];

	return NULL;
}

static ulong part_align(struct up_part *part);

/*
 * Everything that can be checked before answering is checked here, so a
 * refused upload is refused while the browser is still listening.  Once the
 * response is out the page has already said "done"; a failure after that
 * only shows on the console and the panel.
 */
static int httpd_validate(void)
{
	static char part_name[] = UBI_PART;
	char buf[UBIVOL_NAME_MAX];
	struct up_part *p;
	int nvols = 0;
	int needs_ubi;
	int i;

	for (i = 0; i < up_nparts; i++) {
		struct fvol *v = fvol_find(up_parts[i].name);

		if (v)
			nvols++;
		/*
		 * A backup read out of the volume is longer: a dynamic volume
		 * reads back as all of its LEBs, and those past the data are
		 * erased.  Taken as the same file cut to size -- but only when
		 * every byte past it is 0xff, or a wrong file of about the
		 * right length would get in the same way.
		 */
		if (v && up_parts[i].size > v->size) {
			const u8 *d = (const u8 *)part_align(&up_parts[i]);

			if (all_ff(d + v->size, up_parts[i].size - v->size)) {
				printf("httpd: %s is %u bytes, erased past %u; "
				       "writing the first %u\n",
				       up_parts[i].name, up_parts[i].size,
				       v->size, v->size);
				up_parts[i].size = v->size;
			}
		}
		if (v && up_parts[i].size != v->size) {
			httpd_reject("%s is %u bytes, the %s volume holds "
				     "exactly %u (a longer backup of the "
				     "volume is taken if all past that is "
				     "0xff)", up_parts[i].name,
				     up_parts[i].size, v->name, v->size);
			return -1;
		}
	}

	/*
	 * Both write recipes erase before they write, so a file that does
	 * not fit leaves the target empty rather than untouched.  The
	 * factory volumes have had an exact-size gate all along; these two
	 * never did, and the page could not gate them either -- it takes
	 * the partition and volume sizes to know.
	 */
	p = part_find(FIELD_BL2);
	if (p) {
		struct mtd_info *m = get_mtd_device_nm(BL2_PART);
		u64 room = 0;

		/* Put it straight back: a held usecount is what stops a
		 * partition from being torn down later. */
		if (!IS_ERR(m)) {
			room = m->size - BL2_IMAGE_OFF;
			put_mtd_device(m);
		}
		if (room && p->size > room) {
			httpd_reject("the BL2 is %u bytes; the " BL2_PART
				     " partition holds %llu from 0x%x",
				     p->size, (unsigned long long)room,
				     BL2_IMAGE_OFF);
			return -1;
		}
	}

	/*
	 * Rebuilding ubi erases the fip volume with everything else, so the
	 * upload has to bring back both halves of the boot chain.  The page
	 * refuses this too, but the page is not the only client.
	 *
	 * The FIP is the obvious half.  The BL2 is the one that was missing,
	 * and on a board that has not been migrated yet its absence is a
	 * brick: our bl2 partition is 0x0-0x20000 with ubi starting right
	 * after it, while the factory bootloader partition runs 0x0-0x80000.
	 * So "mtd erase ubi" takes out everything the factory BL2 loads after
	 * itself, and what is left is a factory BL2 that comes up, finds its
	 * next stage replaced by UBI, and stops -- with our FIP sitting in a
	 * volume it has never heard of.  Only a serial cable gets that board
	 * back, which is exactly what this page exists to avoid.
	 *
	 * Demanded unconditionally rather than only when UBI will not attach.
	 * Deciding by attach state would mean a full scan of the chip from
	 * inside an upload handler, and would buy only one 120 KiB file in
	 * the rare case -- a board already on this layout that just wants its
	 * UBI rebuilt.  The guide has always said to send all three for a
	 * migration; this makes the page say the same thing.
	 */
	if (part_find(FIELD_FORMAT) && !part_find(FIELD_FIP)) {
		httpd_reject("rebuilding UBI without a U-Boot FIP would leave "
			     "nothing to boot");
		return -1;
	}
	if (part_find(FIELD_FORMAT) && !part_find(FIELD_BL2)) {
		httpd_reject("rebuilding UBI erases what a factory BL2 loads "
			     "after itself; upload the BL2 preloader too");
		return -1;
	}

	/*
	 * Try-boot writes nothing, so anything else in the same upload would
	 * either be silently dropped or turn it into a write after all.
	 * Neither is what the box said, so refuse instead.
	 */
	if (part_find(FIELD_TRYBOOT)) {
		if (!part_find(FIELD_FIT)) {
			httpd_reject("try-boot needs a firmware image");
			return -1;
		}
		if (up_nparts > 2) {
			httpd_reject("try-boot takes the firmware alone; it writes "
				     "nothing, so nothing else can come with it");
			return -1;
		}

		return 0;
	}

	/*
	 * One shape of write per upload, because the state machine only ever
	 * takes one.  An upload carrying a volume attaches UBI, writes the
	 * volumes and goes straight to the read-back -- a bl2, fip or fit
	 * sent alongside is never written at all, and then the read-back
	 * reports a mismatch against a target nothing wrote.  Which reads,
	 * to the user, as a flash that has gone bad.
	 *
	 * The page cannot produce this: its forms are separate.  curl can,
	 * and the same reasoning as try-boot above applies -- the honest
	 * place to say so is before the upload is accepted.
	 */
	if ((nvols || part_find(FIELD_UBIVOL_FILE)) &&
	    (part_find(FIELD_BL2) || part_find(FIELD_FIP) ||
	     part_find(FIELD_FIT) || part_find(FIELD_FORMAT))) {
		httpd_reject("volumes and the boot chain are written by two "
			     "different paths; send them as two uploads");
		return -1;
	}

	p = part_find(FIELD_UBIVOL_FILE);
	if (p) {
		if (!part_text(FIELD_UBIVOL_NAME, buf, sizeof(buf)) ||
		    !vol_name_ok(buf)) {
			httpd_reject("missing or invalid UBI volume name");
			return -1;
		}
	}

#if IS_ENABLED(CONFIG_CMD_HTTPD_STOCK_RESTORE)
	p = part_find(FIELD_STOCK);
	if (p) {
		struct mtd_info *m = flash_master();
		char ob[24];
		ulong off = 0;
		int extra = 0;

		/*
		 * Worse than the case above: a whole-flash restore ends the
		 * write where it finishes, so anything else in the upload is
		 * dropped without even a read-back to notice it.  Counted
		 * rather than compared against up_nparts, because the offset
		 * field is optional and "stock plus one more file" would come
		 * to the same two parts as "stock plus its offset".
		 */
		for (i = 0; i < up_nparts; i++)
			if (strcmp(up_parts[i].name, FIELD_STOCK) &&
			    strcmp(up_parts[i].name, FIELD_STOCK_OFF))
				extra++;

		if (extra) {
			httpd_reject("a whole-flash restore replaces "
				     "everything; nothing else can come with "
				     "it");
			return -1;
		}
		if (!m) {
			httpd_reject("no flash device found");
			return -1;
		}
		if (part_text(FIELD_STOCK_OFF, ob, sizeof(ob)))
			off = hextoul(ob, NULL);
		if (off % m->erasesize) {
			httpd_reject("offset 0x%lx is not a multiple of the "
				     "erase block (0x%x)", off, m->erasesize);
			return -1;
		}
		if ((u64)off + p->size > m->size) {
			httpd_reject("offset 0x%lx + %u bytes exceeds the "
				     "flash (%llu bytes)", off, p->size,
				     (unsigned long long)m->size);
			return -1;
		}
	}
#endif

	/*
	 * What the flash is in a state to take.  The page checks the same
	 * things against its copy of /info, but the page is not the only
	 * client and its copy can be stale or missing, so the answer has to
	 * come from the flash as it is now -- and before the 200 goes out,
	 * because after that the only place a failure shows is the serial
	 * console.
	 *
	 * A whole-flash restore replaces everything and is exempt.  A rebuild
	 * needs nothing from the UBI that is there (and brings its own FIP,
	 * checked above).  Everything else either writes into that UBI, or
	 * ends in a reset that BL2 has to boot from -- which takes a fip
	 * volume, or a FIP in this upload, or no reset at all.
	 */
#if IS_ENABLED(CONFIG_CMD_HTTPD_STOCK_RESTORE)
	if (part_find(FIELD_STOCK))
		return 0;
#else
	/*
	 * Without the whole-flash restore compiled in, httpd_flash() ignores
	 * a "stock" field entirely.  Letting one through here would skip
	 * every check below it and then quietly do nothing at all, which
	 * reads to the user as a successful restore.
	 */
	if (part_find(FIELD_STOCK)) {
		httpd_reject("this build has no whole-flash restore");
		return -1;
	}
#endif
	if (part_find(FIELD_FORMAT)) {
		/* The volume is about to be recreated at the size
		 * ubi_write_fip uses, so that is the only bound there is. */
		p = part_find(FIELD_FIP);
		if (p && p->size > FIP_VOL_BYTES) {
			httpd_reject("the U-Boot FIP is %u bytes; a rebuilt fip "
				     "volume holds %u", p->size, FIP_VOL_BYTES);
			return -1;
		}

		return 0;
	}

	needs_ubi = part_find(FIELD_FIP) || part_find(FIELD_FIT) ||
		    part_find(FIELD_UBIVOL_FILE) || nvols;

	if (ubi_part(part_name, NULL) && needs_ubi) {
		httpd_reject("no usable UBI on the flash to write into: "
			     "tick \"rebuild UBI\" and upload BL2, U-Boot "
			     "and firmware together");
		return -1;
	}

	/* UBI is up by now, so the volume can be measured rather than
	 * assumed: an in-place write is bounded by what it reserved. */
	p = part_find(FIELD_FIP);
	if (p) {
		struct ubi_device *ubi = ubi_get_device(0);
		u64 room = 0;

		if (ubi) {
			struct ubi_volume *v = ubi_vol_find(ubi, "fip");

			room = v ? (u64)v->reserved_pebs * ubi->leb_size
				 : (u64)FIP_VOL_BYTES;
			ubi_put_device(ubi);
		}
		if (room && p->size > room) {
			httpd_reject("the U-Boot FIP is %u bytes; the fip volume "
				     "holds %llu", p->size,
				     (unsigned long long)room);
			return -1;
		}
	}

	/*
	 * A factory volume that is not there gets created at its exact size,
	 * and an installed board has no room for one: rootfs_data took all
	 * that was left the first time it booted.  The create would fail
	 * after the 200 is out, as a line on the serial console; say it now,
	 * with the way out -- clearing the settings removes rootfs_data, and
	 * the next boot makes it again from whatever the volume leaves.
	 */
	if (nvols) {
		struct ubi_device *ubi = ubi_get_device(0);
		const char *missing = NULL;
		u64 need = 0, avail = 0;

		if (ubi) {
			avail = (u64)ubi->avail_pebs * ubi->leb_size;
			for (i = 0; i < up_nparts; i++) {
				struct fvol *v = fvol_find(up_parts[i].name);

				if (!v || ubi_vol_find(ubi, v->name))
					continue;
				if (!missing)
					missing = v->name;
				need += (u64)DIV_ROUND_UP(v->size,
							  ubi->leb_size) *
					ubi->leb_size;
			}
			ubi_put_device(ubi);
		}
		if (need > avail) {
			httpd_reject("the %s volume is not there, and the UBI "
				     "has %llu bytes free for the %llu it "
				     "takes: use \"Clear system settings\" "
				     "first, then write it again", missing,
				     (unsigned long long)avail,
				     (unsigned long long)need);
			return -1;
		}
	}

	/*
	 * What used to be refused here -- writing something onto a board
	 * that would not boot afterwards -- was refused because the write
	 * ended in a reset, so an upload the page called a success turned
	 * into a board that never came back.  It does not reset any more:
	 * the write finishes, the page is still connected, and it says what
	 * is still missing before offering the reboot.  Refusing the write
	 * outright would only stop the user from putting the first of two
	 * files in place.
	 */
	return 0;
}

/*
 * Body fully received: walk the multipart stream once and record where each
 * form field's payload landed.  Nothing is copied -- the parts are flashed in
 * place, which is what stops a 235 MB upload from needing a second 235 MB.
 */
static void httpd_finish(void)
{
	char *base = (char *)up_base;
	const char *sep = up_bound + 2;		/* "--" + boundary */
	int seplen = up_bound_len - 2;
	u32 pos = up_body;

	up_nparts = 0;

	while (up_nparts < MAX_PARTS && pos < up_total) {
		char ph[512];
		struct up_part *part;
		const char *q;
		int off, hoff, dlen, n;

		off = mem_find(base + pos, up_total - pos, sep, seplen);
		if (off < 0)
			break;
		pos += off + seplen;

		/* "--" right after the delimiter closes the body. */
		if (up_total - pos >= 2 && !memcmp(base + pos, "--", 2))
			break;
		if (up_total - pos >= 2 && !memcmp(base + pos, "\r\n", 2))
			pos += 2;

		n = up_total - pos;
		if (n > (int)sizeof(ph) - 1)
			n = sizeof(ph) - 1;
		hoff = mem_find(base + pos, n, "\r\n\r\n", 4);
		if (hoff < 0)
			break;
		memcpy(ph, base + pos, hoff);
		ph[hoff] = '\0';
		pos += hoff + 4;

		dlen = mem_find(base + pos, up_total - pos, up_bound,
				up_bound_len);
		if (dlen < 0)
			break;

		/* An untouched <input type=file> arrives with an empty body. */
		q = strstr(ph, "name=\"");
		if (q && dlen > 0) {
			part = &up_parts[up_nparts];
			q += 6;
			for (n = 0; n < (int)sizeof(part->name) - 1 &&
				    q[n] && q[n] != '"'; n++)
				part->name[n] = q[n];
			part->name[n] = '\0';
			part->addr = (ulong)(base + pos);
			part->size = dlen;

			up_nparts++;

			printf("httpd: \"%s\": %u bytes at 0x%lx\n",
			       part->name, part->size, part->addr);
		}

		pos += dlen;		/* now sitting on the next delimiter */
	}

	if (!up_nparts) {
		httpd_reject("no usable form fields in the upload");
		return;
	}

	if (httpd_validate())
		return;

	up_ready = 1;
}

/*
 * ---- GET /dump -----------------------------------------------------------
 *
 * Reads a UBI volume or a raw flash range into memory and hands it to the
 * browser as a file.  What it is for is the window before a first migration:
 * the flash still holds the factory content, ri and bosa are per-unit and
 * unobtainable once "rebuild UBI" has run, and the machine may already have
 * no working system left to dd from.  It is also the only way to produce the
 * image that "back to stock" asks for.
 *
 * It streams.  A whole 256 MiB chip does not fit in RAM beside U-Boot on a
 * 512 MiB board, and the obvious way out -- hand the user several files and
 * let them stitch the thing back together -- is not a feature, it is our
 * memory budget wearing a hat.  So only a window is held at a time and it is
 * refilled as the browser drains it: Content-Length is still the full
 * length, so one click is still one file, whatever its size.
 *
 * tx() is a pull at an arbitrary offset, which is exactly the shape this
 * needs.  A retransmit inside the window is a memcpy; one behind it refills
 * from the checkpoint for that window, which costs a re-read and nothing
 * else.  Refilling blocks the net loop for as long as one window takes to
 * read -- a few hundred milliseconds -- which TCP simply waits out.
 */
#define DUMP_HDR_SZ	512
#define DUMP_WIN_MAX	(8 << 20)

static char	dump_hdr[DUMP_HDR_SZ];
static int	dump_hdr_len;
static ulong	dump_base;	/* the window, not the whole thing */
static u32	dump_len;	/* total output bytes */
static u32	dump_win;	/* window capacity */
static u32	dump_wpos;	/* output offset the window starts at */
static u32	dump_wlen;	/* valid bytes in it, 0 when empty */
static int	dump_raw;
static u64	dump_off;	/* flash offset output offset 0 maps to */
static char	dump_vol[UBIVOL_NAME_MAX];
static struct mtd_info	*dump_mtd;
static char	dump_qs[160];

/*
 * When bytes last actually went out.  A download that the browser abandons
 * -- cancelled, tab closed, cable pulled -- never reaches the end, so
 * nothing would ever clear dump_busy and every later upload would be
 * refused until the page was reloaded.  After this long without progress
 * the next request takes the buffer over.  Cutting a transfer that was only
 * paused cannot hand anybody a bad backup: the crc32 is folded in on the
 * forward pass alone, so /dumpinfo simply never reports it.
 */
#define DUMP_IDLE_MS	15000

static ulong	dump_t0;

/*
 * Blocks the forward pass could not read.  Recorded monotonically so a
 * retransmit cannot count one twice; the list is capped but the count is
 * not, and /dumpinfo reports it -- a backup with holes in it is still worth
 * having, but only if you are told where they are.
 */
#define DUMP_HOLES	16

static u64	dump_hole[DUMP_HOLES];
static int	dump_nhole;
static u64	dump_hole_hi;

/* Rolling over the forward pass, so the figure covers what was really sent. */
static u32	dump_crc;
static u32	dump_crc_pos;

/*
 * The high-water mark of what tx() has handed to the stack.  A pull
 * callback is asked for arbitrary offsets -- a retransmit asks for one
 * already served -- so progress is the highest offset reached, never the
 * last one asked for.
 *
 * /dumpinfo carries it, but nothing can read it while the transfer runs, and
 * the comment here used to claim otherwise.  net/tcp.c holds one
 * "static struct tcp_stream" and refuses a new SYN until the old stream is
 * CLOSED -- and the download holds that stream from the browser's first byte
 * to its last.  So the page gets no answer at all until the file is done, at
 * which point this number is no longer news; the page says so rather than
 * showing a percentage that would never move.  It is still counted, and
 * still correct, for the day tcp.c grows a second stream.
 */
static u32	dump_sent;

/*
 * There is one staging area and one header buffer, so a second download
 * would take both away from the first.  Refuse it out of a buffer of its
 * own rather than through dump_fail(), which would write over exactly the
 * header the transfer in flight is still sending.
 */
/*
 * An upload streams into $loadaddr and would run straight through any gap
 * into the staged download, so the two cannot overlap.  Refusing is the
 * honest answer: silently truncating the download would hand the user a
 * corrupt backup they believe is good.
 */
static const char resp_postbusy[] =
	"HTTP/1.0 400 Bad Request\r\n"
	"Content-Type: text/plain; charset=utf-8\r\n"
	"Connection: close\r\n"
	"\r\n设备正在传输备份。请等待传输完成后再上传，或刷新页面放弃该次下载\n";

static const char resp_dumpbusy[] =
	"HTTP/1.0 503 Service Unavailable\r\n"
	"Content-Type: text/plain; charset=utf-8\r\n"
	"Connection: close\r\n"
	"\r\n另一个备份正在传输，请等待传输完成后重试\n";

/*
 * Reachable only because 0.3.0 keeps serving while it writes.  Everything
 * answered with this either writes the environment volume underneath a UBI
 * write or moves the address out from under the page watching that write.
 */
static const char resp_wrbusy[] =
	"HTTP/1.0 503 Service Unavailable\r\n"
	"Content-Type: text/plain; charset=utf-8\r\n"
	"Connection: close\r\n"
	"\r\n正在写入闪存，这一项要等写完再改\n";

/* A download nothing is pulling on any more; see DUMP_IDLE_MS. */
static int dump_stale(void)
{
	return dump_busy && get_timer(dump_t0) > DUMP_IDLE_MS;
}

/* Refuse with a reason the page can show; the console gets it too. */
/*
 * One message, two languages: the console gets English, the page gets the
 * Chinese its dictionary knows how to translate.  Both formats take the same
 * arguments in the same order.
 */
static void fmt2(char *en, int enlen, char *zh, int zhlen,
		 const char *fen, const char *fzh, va_list ap)
{
	va_list aq;

	va_copy(aq, ap);
	vsnprintf(en, enlen, fen, ap);
	vsnprintf(zh, zhlen, fzh, aq);
	va_end(aq);
}

static void dump_fail(const char *status, const char *fen, const char *fzh,
		      ...)
{
	char con[128], msg[192];
	va_list ap;

	va_start(ap, fzh);
	fmt2(con, sizeof(con), msg, sizeof(msg), fen, fzh, ap);
	va_end(ap);

	printf("httpd: /dump refused: %s\n", con);

	dump_len = 0;
	dump_wlen = 0;
	dump_busy = 0;
	dump_hdr_len = snprintf(dump_hdr, sizeof(dump_hdr),
				"HTTP/1.0 %s\r\n"
				"Content-Type: text/plain; charset=utf-8\r\n"
				"Connection: close\r\n"
				"\r\n%s\n", status, msg);
}

/* "Nokia XG-040G-MD" + "ri" -> "nokia-xg-040g-md-ri.bin". */
static void dump_name(char *dst, int max, const char *what)
{
	const char *model = fdt_getprop(gd->fdt_blob, 0, "model", NULL);
	int n = 0;

	while (model && *model && n < max - 40) {
		char c = *model++;

		if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			dst[n++] = c;
		else if (n && dst[n - 1] != '-')
			dst[n++] = '-';
	}
	if (n && dst[n - 1] != '-')
		dst[n++] = '-';
	snprintf(dst + n, max - n, "%s.bin", what);
}

/*
 * Read len bytes of flash at offset off, dd style: the buffer pointer and the
 * flash offset move together, always, so a byte's place in the file is its
 * place on the chip.  Nothing here consults the bad block table -- a block
 * marked bad may still hold readable data, and refusing to look would be us
 * deciding what somebody's backup is allowed to contain.
 *
 * Only a read that actually fails yields 0xff, because at that point there is
 * nothing else to hand over.  It is noted rather than fatal: one unreadable
 * block must not throw away the other 235 MiB.
 */
static int dump_read_raw(u64 off, u32 len, u8 *buf)
{
	struct mtd_info *m = dump_mtd;

	if (off + len > m->size)
		return -1;

	while (len) {
		u64 blk = off & ~((u64)m->erasesize - 1);
		size_t rl = 0;
		u32 n = m->erasesize - (u32)(off - blk);
		int ret;

		if (n > len)
			n = len;

		ret = mtd_read(m, off, n, &rl, buf);
		if (ret && ret != -EUCLEAN) {
			memset(buf, 0xff, n);
			if (blk >= dump_hole_hi) {
				if (dump_nhole < DUMP_HOLES)
					dump_hole[dump_nhole] = blk;
				dump_nhole++;
				dump_hole_hi = blk + m->erasesize;
				printf("httpd: /dump cannot read the block at 0x%llx (%d), filling 0xff\n",
				       blk, ret);
			}
		}

		buf += n;
		off += n;
		len -= n;
	}

	return 0;
}

/* Bring the window holding output offset `want` into memory. */
static int dump_fill(u32 want)
{
	u32 pos = (want / dump_win) * dump_win;
	u32 len = dump_len - pos;

	if (dump_wlen && pos == dump_wpos)
		return 0;
	if (len > dump_win)
		len = dump_win;

	if (!dump_raw) {
		if (ubi_volume_read(dump_vol, (char *)dump_base, pos, len)) {
			printf("httpd: /dump reading %s at %u failed\n",
			       dump_vol, pos);
			return -1;
		}
	} else if (dump_read_raw(dump_off + pos, len, (u8 *)dump_base)) {
		printf("httpd: /dump reading flash at 0x%llx failed\n",
		       dump_off + pos);
		return -1;
	}

	dump_wpos = pos;
	dump_wlen = len;

	/*
	 * Fold into the checksum only on the forward pass, so a retransmit
	 * cannot count the same bytes twice.  By the end it covers exactly
	 * what went out, which is a stronger statement than checksumming a
	 * staging buffer would have been.
	 */
	if (pos == dump_crc_pos) {
		dump_crc = crc32(dump_crc, (const u8 *)dump_base, len);
		dump_crc_pos = pos + len;
	}

	return 0;
}

static void httpd_dump(void)
{
	static char part_name[] = UBI_PART;
	char val[UBIVOL_NAME_MAX], what[80], name[128];
	ulong top, avail;
	u64 off = 0;
	u32 len = 0;
	int i;

	dump_busy = 1;
	dump_t0 = get_timer(0);
	dump_wlen = 0;
	dump_sent = 0;
	dump_crc = 0;
	dump_crc_pos = 0;
	dump_nhole = 0;
	dump_hole_hi = 0;

	if (up_active) {
		dump_fail("503 Service Unavailable",
			  "an upload is in progress",
			  "设备正在接收上传，请等待写入完成后再备份");
		return;
	}

	dump_mtd = flash_master();
	if (!dump_mtd) {
		dump_fail("500 Internal Server Error", "no flash device",
			  "没有找到闪存设备");
		return;
	}

	if (qs_get(dump_qs, "vol", val, sizeof(val))) {
		struct ubi_device *ubi;
		struct ubi_volume *v;

		if (!vol_name_ok(val)) {
			dump_fail("400 Bad Request", "bad volume name",
				  "卷名不合法");
			return;
		}
		if (ubi_part(part_name, NULL)) {
			dump_fail("500 Internal Server Error",
				  "UBI does not attach; only raw offsets can be read",
				  "UBI 无法挂载，只能按 flash 偏移备份");
			return;
		}
		ubi = ubi_get_device(0);
		if (!ubi) {
			dump_fail("500 Internal Server Error",
				  "UBI attached but not accessible",
				  "UBI 已挂载但无法访问");
			return;
		}
		v = ubi_vol_find(ubi, val);
		if (!v) {
			ubi_put_device(ubi);
			dump_fail("404 Not Found", "no volume named %s",
				  "没有名为 %s 的卷", val);
			return;
		}
		/*
		 * used_bytes is the real length of a static volume and the
		 * whole reservation of a dynamic one -- the same figure the
		 * page put on the button, so what arrives is what it said.
		 */
		len = v->used_bytes > 0 ? (u32)v->used_bytes :
			(u32)((u64)v->reserved_pebs * ubi->leb_size);
		ubi_put_device(ubi);
		/*
		 * Except a factory volume: its data is the configured length
		 * and the rest of the last LEB is erased.  Cut there, and the
		 * backup is the same file as the stock partition it came from
		 * -- same length, same md5 -- and writes straight back here
		 * or into another project's volume of the same size.
		 */
		for (i = 0; i < nfvols; i++)
			if (!strcmp(fvols[i].name, val) && len > fvols[i].size)
				len = fvols[i].size;

		dump_raw = 0;
		strlcpy(dump_vol, val, sizeof(dump_vol));
		snprintf(what, sizeof(what), "%s", val);
	} else {
		char o[24], l[24];

		dump_raw = 1;
		off = qs_get(dump_qs, "off", o, sizeof(o)) ? hextoul(o, NULL) : 0;
		if (off >= dump_mtd->size) {
			dump_fail("400 Bad Request", "offset past the end of flash",
				  "起始偏移超过闪存容量");
			return;
		}
		/* Every byte from here to the end, bad blocks included. */
		len = qs_get(dump_qs, "len", l, sizeof(l)) ?
			(u32)hextoul(l, NULL) : (u32)(dump_mtd->size - off);
		dump_off = off;
		snprintf(what, sizeof(what), "flash-0x%llx-0x%x", off, len);
	}

	if (!len) {
		dump_fail("400 Bad Request", "zero length", "长度为 0");
		return;
	}
	if (dump_raw && off + len > dump_mtd->size) {
		dump_fail("400 Bad Request", "offset + length past the end of flash",
			  "偏移加长度超过闪存容量");
		return;
	}

	/*
	 * Only a window has to fit, so what used to be a ceiling on the whole
	 * transfer is now just "is there room to work in".
	 */
	top = gd->start_addr_sp - UPLOAD_MARGIN;
	avail = top > up_base + DUMP_GAP ? top - (up_base + DUMP_GAP) : 0;
	dump_win = avail > DUMP_WIN_MAX ? DUMP_WIN_MAX : (u32)avail;
	dump_win &= ~0xffUL;
	/*
	 * Asked of the memory, before the window is narrowed to the
	 * transfer: a 4 KiB volume wants a 4 KiB window, and what is being
	 * asked here is whether there is room to work in at all.  Narrowing
	 * first turned down every object smaller than the floor -- and told
	 * the user it was short of memory, which it was not.
	 */
	if (dump_win < SZ_64K) {
		dump_fail("507 Insufficient Storage",
			  "not enough memory for a read window",
			  "内存不足，无法分配读取窗口");
		return;
	}
	if (dump_win > len)
		dump_win = len;
	dump_base = (top - dump_win) & ~0xffUL;

	dump_len = len;
	dump_wpos = 0;

	/* First window now, so a read error is still a status code. */
	if (dump_fill(0)) {
		dump_fail("500 Internal Server Error", "read failed",
			  "读取失败，详见串口日志");
		return;
	}

	dump_name(name, sizeof(name), what);
	strlcpy(dump_last_name, name, sizeof(dump_last_name));
	printf("httpd: /dump %s, %u bytes, %u KiB window at 0x%lx\n",
	       name, len, dump_win >> 10, dump_base);

	dump_hdr_len = snprintf(dump_hdr, sizeof(dump_hdr),
				"HTTP/1.0 200 OK\r\n"
				"Content-Type: application/octet-stream\r\n"
				"Content-Length: %u\r\n"
				"Content-Disposition: attachment; filename=\"%s\"\r\n"
				"Connection: close\r\n"
				"\r\n", len, name);
}

static char dumpinfo_buf[320];
static int dumpinfo_len;

static int httpd_dumpinfo(void)
{
	struct jbuf jb;
	u32 sent = 0;

	/* The header is not part of what the user asked for; do not count it. */
	if (dump_sent > (u32)dump_hdr_len)
		sent = dump_sent - (u32)dump_hdr_len;
	if (sent > dump_len)
		sent = dump_len;

	jb_init(&jb, dumpinfo_buf, sizeof(dumpinfo_buf));
	jb_printf(&jb, JSON_HDR("200 OK")
		  "{\"seq\":%u,\"len\":%u,\"crc\":\"%08x\",\"holes\":%d"
		  ",\"busy\":%d,\"sent\":%u,\"total\":%u,\"name\":",
		  dump_last_seq, dump_last_len, dump_last_crc, dump_last_holes,
		  dump_busy ? 1 : 0, sent, dump_busy ? dump_len : 0);
	jb_str(&jb, dump_last_name);
	jb_printf(&jb, "}");

	return jb_done(&jb, "/dumpinfo");
}

/* Header first, then the window; a miss refills it and comes back. */
static int dump_tx(u32 off, void *buf, int maxlen)
{
	u32 total = (u32)dump_hdr_len + dump_len;
	u32 want;
	int n;

	if (off >= total)
		return 0;

	if (off < (u32)dump_hdr_len) {
		n = dump_hdr_len - off;
		if (n > maxlen)
			n = maxlen;
		memcpy(buf, dump_hdr + off, n);
		if (off + n > dump_sent)
			dump_sent = off + n;

		return n;
	}

	dump_t0 = get_timer(0);

	want = off - dump_hdr_len;
	if (want < dump_wpos || want >= dump_wpos + dump_wlen) {
		if (dump_fill(want)) {
			/*
			 * Nothing useful can be said at this point: the 200
			 * and the length went out long ago.  Stopping short
			 * makes the browser call it a failed download, which
			 * is the truth; the reason is on the console.
			 */
			printf("httpd: /dump aborted at %u of %u bytes\n",
			       want, dump_len);
			return 0;
		}
	}

	n = dump_wpos + dump_wlen - want;
	if (n > maxlen)
		n = maxlen;
	memcpy(buf, (const void *)(dump_base + (want - dump_wpos)), n);
	if (off + n > dump_sent)
		dump_sent = off + n;

	return n;
}

#if IS_ENABLED(CONFIG_CMD_HTTPD_STOCK_RESTORE)
/*
 * ---- POST /stock?off= ----------------------------------------------------
 *
 * A whole-chip restore does not fit in RAM.  From $loadaddr up to where
 * U-Boot relocated itself is about 249 MiB on a 512 MiB board, and the raw
 * image of a 256 MiB chip is 256 MiB -- so the only way to offer it at all
 * was to make the user cut the file up and write it in pieces.  Splitting is
 * our problem, not theirs.  This path never holds the image: bytes go to
 * flash as they arrive and the size of the upload stops mattering.
 *
 * It is deliberately not multipart.  Every other form on the page carries
 * several fields and is small enough to stage whole, and that code is
 * untouched.  Streaming a multipart body would mean scanning for boundaries
 * incrementally and inferring where the payload stops -- and being two bytes
 * wrong about the closing delimiter puts two bytes of garbage in the last
 * eraseblock.  A raw body leaves no such question: the offset is in the
 * request line and the length is in Content-Length, both known from the
 * first packet.
 *
 * The ring only has to absorb reordering.  The stack drops anything outside
 * [rcv_nxt, rcv_nxt + rcv_wnd) before it ever reaches us, and rcv_wnd is
 * PKTBUFSRX * TCP_MSS -- tens of kilobytes.  Four eraseblocks would do.
 *
 * What is given up is that a failed transfer no longer leaves the flash
 * untouched.  That is what streaming means and it cannot be argued away, so
 * the failure is made survivable instead: no reset on error, the page stays
 * up in RAM, and it says plainly that the flash is now half written and the
 * transfer has to be repeated before the board is rebooted.
 */
#define ST_RING_MAX	(4 << 20)

static struct mtd_info	*st_mtd;
static ulong	st_ring;	/* == up_base; a ring of whole eraseblocks */
static u32	st_blk;		/* eraseblock size */
static u32	st_nblk;	/* slots in the ring */
static u32	st_hdr_end;	/* stream offset of the first body byte */
static u32	st_body;	/* body length, from Content-Length */
static u32	st_done;	/* body bytes already committed to flash */
static u64	st_off;		/* flash offset body byte 0 belongs at */
static u32	st_crc;		/* over the bytes received, to compare locally */
static int	st_nskip;	/* bad blocks passed over */
static u64	st_skip[16];
static int	st_started;	/* at least one eraseblock has been written */
static int	st_failed;
static int	st_ok;
static char	st_resp[512];
static int	st_resp_len;

/*
 * The tail the image does not cover.  Off by default in the protocol sense
 * -- no wipe=1 in the query string, nothing is erased past the image -- so a
 * partition-sized write to an offset stays exactly that.
 */
static int	st_wipe;	/* erase past the image when it is written */
static u64	st_wipe_pos;	/* next eraseblock of the tail */
static u64	st_wipe_end;
static u32	st_wipe_n;	/* blocks erased */
static u32	st_wipe_bad;	/* bad blocks passed over */
static int	st_wipe_in;	/* the tick must not re-enter the erase */

/* The console line in English, the answer in what the page translates. */
static void st_reply(const char *status, const char *con, const char *msg)
{
	printf("httpd: /stock: %s\n", con);
	st_resp_len = snprintf(st_resp, sizeof(st_resp),
			       "HTTP/1.0 %s\r\n"
			       "Content-Type: text/plain; charset=utf-8\r\n"
			       "Connection: close\r\n"
			       "\r\n%s\n", status, msg);
}

/* Refuse, and say whether the flash has already been changed. */
static void st_fail(const char *fen, const char *fzh, ...)
{
	char en[128], zh[192], con[192], msg[256];
	va_list ap;

	va_start(ap, fzh);
	fmt2(en, sizeof(en), zh, sizeof(zh), fen, fzh, ap);
	va_end(ap);

	st_failed = 1;
	if (st_started) {
		snprintf(con, sizeof(con), "%s; flash is partly written, write "
			 "again until it succeeds before rebooting", en);
		snprintf(msg, sizeof(msg), "%s。闪存已写入一部分，此时重启将无法"
			 "启动。请重新写入至成功，其间不要断电", zh);
		st_reply("500 Internal Server Error", con, msg);
	} else {
		snprintf(con, sizeof(con), "%s; flash untouched", en);
		snprintf(msg, sizeof(msg), "%s（闪存尚未改动）", zh);
		st_reply("400 Bad Request", con, msg);
	}
}

/*
 * Parse the head, which the CONN_UNKNOWN path has already staged at up_base,
 * and lay the ring out over the same area.  Everything that can be refused is
 * refused here -- once the first eraseblock is gone there is no going back.
 *
 * 0 when the ring is laid out and the write can start, -1 when the request is
 * refused (the answer is written and waiting), 1 when the head is not all here
 * yet and the caller should ask again on the next segment.
 */
static int st_begin(u32 rx_bytes)
{
	char hdr[HDRBUF_SZ];
	char qs[96], o[24];
	const char *p, *sp;
	int hdr_end, n, qlen = 0;
	ulong avail, clen;
	u32 body_here;

	st_hdr_end = st_body = st_done = st_crc = 0;
	st_nskip = st_started = st_failed = st_ok = 0;
	st_wipe = st_wipe_in = st_wipe_n = st_wipe_bad = 0;
	st_resp_len = 0;

	n = rx_bytes < HDRBUF_SZ - 1 ? rx_bytes : HDRBUF_SZ - 1;
	memcpy(hdr, (const void *)up_base, n);
	hdr[n] = '\0';

	hdr_end = mem_find(hdr, n, "\r\n\r\n", 4);
	if (hdr_end < 0) {
		/*
		 * Only a failure once there is no room left to find it
		 * in: a head can span segments, and the caller asks
		 * again on the next one.  httpd_parse() draws the same
		 * line in the same place.
		 */
		if (n >= HDRBUF_SZ - 1) {
			st_fail("request head over %d bytes",
				"请求头超过 %d 字节", HDRBUF_SZ - 1);
			return -1;
		}

		return 1;
	}
	hdr_end += 4;

	/* "POST /stock?off=0x0 HTTP/1.1" */
	p = memchr(hdr, '?', hdr_end);
	if (p) {
		sp = memchr(p, ' ', hdr_end - (p - hdr));
		qlen = sp ? (int)(sp - p) - 1 : 0;
		if (qlen > (int)sizeof(qs) - 1)
			qlen = sizeof(qs) - 1;
		if (qlen > 0)
			memcpy(qs, p + 1, qlen);
	}
	qs[qlen > 0 ? qlen : 0] = '\0';
	st_off = qs_get(qs, "off", o, sizeof(o)) ? hextoul(o, NULL) : 0;
	st_wipe = qs_get(qs, "wipe", o, sizeof(o)) && o[0] == '1';

	p = strstr(hdr, "Content-Length:");
	if (!p) {
		st_fail("no Content-Length", "没有 Content-Length");
		return -1;
	}
	p += 15;
	while (*p == ' ' || *p == '\t')
		p++;
	clen = simple_strtoul(p, NULL, 10);
	if (!clen) {
		st_fail("zero length", "长度为 0");
		return -1;
	}

	st_mtd = flash_master();
	if (!st_mtd) {
		st_fail("no flash device", "没有找到闪存设备");
		return -1;
	}
	st_blk = st_mtd->erasesize;

	if (st_off & (u64)(st_blk - 1)) {
		st_fail("offset 0x%llx is not aligned to the 0x%x erase block",
			"写入偏移 0x%llx 未按擦除块 0x%x 对齐", st_off, st_blk);
		return -1;
	}
	if (st_off + clen > st_mtd->size) {
		st_fail("0x%llx + %lu bytes runs past the %llu byte flash",
			"自 0x%llx 起写入 %lu 字节将超出闪存容量 %llu",
			st_off, clen, (unsigned long long)st_mtd->size);
		return -1;
	}

	avail = upload_max();
	st_nblk = (u32)(avail / st_blk);
	if (st_nblk > ST_RING_MAX / st_blk)
		st_nblk = ST_RING_MAX / st_blk;
	if (st_nblk < 4) {
		st_fail("not enough memory for the receive ring, need %u bytes",
			"内存不足以分配接收环，至少需要 %u 字节", 4 * st_blk);
		return -1;
	}
	st_ring = up_base;

	st_hdr_end = (u32)hdr_end;
	st_body = (u32)clen;

	/*
	 * From the block after the last one the image lands in.  That last
	 * block was erased whole and only partly written, so its own tail is
	 * already blank; starting here is what keeps the two from overlapping.
	 */
	st_wipe_pos = st_off + ((clen + st_blk - 1) & ~(ulong)(st_blk - 1));
	st_wipe_end = st_mtd->size;
	if (st_wipe_pos >= st_wipe_end)
		st_wipe = 0;
	if (st_wipe)
		printf("httpd: /stock: will erase 0x%llx..0x%llx after the "
		       "image\n", st_wipe_pos, st_wipe_end);

	/*
	 * The head already carried some body bytes; move them into slot 0.
	 * Up to up_hi rather than rx_bytes: a segment that arrived ahead of
	 * a hole while the head was still incomplete was staged at its
	 * stream offset, and TCP has already counted it received, so it is
	 * never sent again.  Left behind, it would sit st_hdr_end bytes away
	 * from its ring slot and st_put() would write whatever was there.
	 * The hole in between is moved too and filled later by st_rx().
	 */
	if (up_hi > rx_bytes)
		rx_bytes = up_hi;
	body_here = rx_bytes > st_hdr_end ? rx_bytes - st_hdr_end : 0;
	if (body_here > st_nblk * st_blk)
		body_here = st_nblk * st_blk;
	if (body_here)
		memmove((void *)st_ring,
			(const void *)(up_base + st_hdr_end), body_here);

	printf("httpd: /stock: %lu bytes to 0x%llx, ring %u x %u KiB at 0x%lx\n",
	       clen, st_off, st_nblk, st_blk >> 10, st_ring);

	return 0;
}

/* One eraseblock of the image, at the offset it belongs at. */
static int st_put(u32 k, u32 n)
{
	u64 pos = st_off + (u64)k * st_blk;
	const u8 *p = (const u8 *)(st_ring + (k % st_nblk) * st_blk);
	struct erase_info ei;
	size_t wl = 0;
	int ret;

	/* Over what arrived, so it can be compared with the file on disk. */
	st_crc = crc32(st_crc, p, n);

	/*
	 * The isbad test is load bearing, exactly as in flash_raw():
	 * nanddev_erase() clears a bad block's BBT entry and erases it anyway,
	 * wiping the marker in its OOB.  Nothing may reach mtd_erase() without
	 * passing this first.
	 */
	if (mtd_block_isbad(st_mtd, pos)) {
		if (st_nskip < (int)ARRAY_SIZE(st_skip))
			st_skip[st_nskip] = pos;
		st_nskip++;

		return 0;
	}

	memset(&ei, 0, sizeof(ei));
	ei.mtd = st_mtd;
	ei.addr = pos;
	ei.len = st_blk;
	ret = mtd_erase(st_mtd, &ei);
	if (ret) {
		st_fail("erase at 0x%llx failed (%d)",
			"擦除 0x%llx 失败（%d）", pos, ret);
		return -1;
	}

	ret = mtd_write(st_mtd, pos, n, &wl, p);
	if (ret || wl != n) {
		st_fail("write at 0x%llx failed (%d, %u of %u written)",
			"写入 0x%llx 失败（%d，实际写入 %u/%u）", pos, ret,
			(u32)wl, n);
		return -1;
	}

	return 0;
}

/* The last thing either path does: stop the chase, answer, stand still. */
static void st_finish(void)
{
	char ok[80];
	int i;

	/* Dark, which is what every other write ends on too. */
	httpd_tick_stop();
	st_ok = 1;

	if (st_nskip) {
		printf("httpd: /stock: %d bad block(s) skipped, what the image "
		       "held for them was not written:", st_nskip);
		for (i = 0; i < st_nskip && i < (int)ARRAY_SIZE(st_skip); i++)
			printf(" 0x%llx", st_skip[i]);
		printf("%s\n", st_nskip > (int)ARRAY_SIZE(st_skip) ? " ..." : "");
		printf("httpd: /stock: everything else landed at its own offset\n");
	}
	if (st_wipe)
		snprintf(ok, sizeof(ok),
			 "ok %u bytes crc32 %08x skipped %d wiped %u",
			 st_body, st_crc, st_nskip, st_wipe_n);
	else
		snprintf(ok, sizeof(ok), "ok %u bytes crc32 %08x skipped %d",
			 st_body, st_crc, st_nskip);
	st_reply("200 OK", ok, ok);
}

/*
 * The tail, one budget of eraseblocks per tick.
 *
 * Erasing it in a single call is what the straight-line version would do,
 * and on a 256 MiB chip behind a small image that is thousands of blocks
 * with the /stock connection held open throughout: nothing would answer an
 * ACK, the peer would retransmit into a stack that is not running, and the
 * watchdog would have its own opinion.  So it steps off httpd_tick() like
 * every other long write here, and the board keeps answering in between.
 *
 * There is no progress to poll for meanwhile.  The board has one
 * tcp_stream and /stock is holding it, so /wr cannot be asked anything
 * until this is over; the page waits on the 200, which carries the count,
 * and the running commentary goes to the console.
 */
#define ST_WIPE_PER_TICK	16

static int st_wipe_step(void)
{
	int n;

	if (!st_wipe || st_failed || st_ok || !st_body)
		return 0;
	/* The image comes first; this only ever runs behind a whole one. */
	if (st_done < st_body || st_wipe_in)
		return 0;

	st_wipe_in = 1;
	for (n = 0; n < ST_WIPE_PER_TICK && st_wipe_pos < st_wipe_end; n++) {
		struct erase_info ei;
		int ret;

		/*
		 * Same reason as st_put(): erasing a bad block clears its
		 * marker out of the OOB and loses it for good.
		 */
		if (mtd_block_isbad(st_mtd, st_wipe_pos)) {
			st_wipe_bad++;
			st_wipe_pos += st_blk;

			continue;
		}

		memset(&ei, 0, sizeof(ei));
		ei.mtd = st_mtd;
		ei.addr = st_wipe_pos;
		ei.len = st_blk;
		ret = mtd_erase(st_mtd, &ei);
		if (ret) {
			/*
			 * The image is already whole and the board will boot.
			 * Failing the request here would tell the page to
			 * repeat the transfer, which is the wrong advice for
			 * housekeeping that did not finish.  Say so and stop.
			 */
			printf("httpd: /stock: erasing the tail stopped at "
			       "0x%llx (%d); the image itself is complete\n",
			       st_wipe_pos, ret);
			st_wipe_end = st_wipe_pos;

			break;
		}
		st_wipe_n++;
		st_wipe_pos += st_blk;
	}
	st_wipe_in = 0;

	if (st_wipe_pos < st_wipe_end)
		return 0;

	printf("httpd: /stock: tail erased, %u block(s), %u bad passed "
	       "over\n", st_wipe_n, st_wipe_bad);
	st_finish();

	/* st_finish() stopped the clock; re-arming it would undo that. */
	return 1;
}

/* Commit whatever whole blocks the contiguous prefix now covers. */
static void st_commit(u32 rx_bytes)
{
	u32 have;

	if (st_failed || st_ok || !st_body)
		return;

	have = rx_bytes > st_hdr_end ? rx_bytes - st_hdr_end : 0;
	if (have > st_body)
		have = st_body;
	if (have < st_done)
		return;

	while (have - st_done >= st_blk ||
	       (have == st_body && st_done < st_body)) {
		u32 n = have - st_done;

		if (n > st_blk)
			n = st_blk;
		if (!st_started) {
			st_started = 1;
			/*
			 * The chase stays on, and that is the whole answer
			 * here.  Every other write blinks because by then the
			 * upload is over and the cable is free; this one
			 * writes out of a connection that is still arriving,
			 * so the cable is exactly what must not be touched --
			 * which is what the chase has always meant.  Blinking
			 * would say the opposite, and the page is already
			 * showing a progress bar for "how far along is it".
			 */
			printf("httpd: /stock: writing now, do not power off\n");
		}
		if (st_put(st_done / st_blk, n))
			return;
		st_done += n;

		if (!(st_done & ((32 << 20) - 1)))
			printf("httpd: /stock: %u of %u bytes\n", st_done,
			       st_body);
	}

	if (st_done < st_body)
		return;

	/*
	 * The image is whole.  If the tail is to go too, the chase stays on
	 * and httpd_tick() takes it from here -- nothing else is arriving on
	 * this connection, so there is nothing left to drive it from.
	 */
	if (st_wipe && st_wipe_pos < st_wipe_end) {
		printf("httpd: /stock: image written, erasing the %llu KiB "
		       "after it\n", (st_wipe_end - st_wipe_pos) >> 10);

		return;
	}

	st_finish();
}

/*
 * Body bytes into the ring.  The return value is how many bytes counting from
 * the start of this segment the stack may treat as received, so everything
 * refused has to be a suffix -- which it is: the only reason to refuse is
 * running past the far edge of the ring.
 */
static int st_rx(u32 rx_offs, const u8 *src, int len)
{
	u32 b, cap, pre = 0, skip;

	if (st_failed || st_ok)
		return len;		/* swallow the rest of the upload */

	/* Bytes of the head; the CONN_UNKNOWN path already took those. */
	if (rx_offs < st_hdr_end) {
		skip = st_hdr_end - rx_offs;
		if ((u32)len <= skip)
			return len;
		pre += skip;
		src += skip;
		len -= skip;
		b = 0;
	} else {
		b = rx_offs - st_hdr_end;
	}

	/* Already committed to flash; its ring slot has moved on. */
	if (b < st_done) {
		skip = st_done - b;
		if ((u32)len <= skip)
			return (int)pre + len;
		pre += skip;
		src += skip;
		len -= skip;
		b = st_done;
	}

	cap = st_done + st_nblk * st_blk;
	if (b >= cap)
		return (int)pre;	/* 0 here means "send it again later" */
	if ((u32)len > cap - b)
		len = (int)(cap - b);

	while (len) {
		u32 k = b / st_blk, io = b % st_blk;
		u32 n = st_blk - io;

		if (n > (u32)len)
			n = (u32)len;
		memcpy((u8 *)(st_ring + (k % st_nblk) * st_blk) + io, src, n);
		src += n;
		b += n;
		len -= n;
		pre += n;
	}

	return (int)pre;
}
#endif /* CONFIG_CMD_HTTPD_STOCK_RESTORE */

static int httpd_rx(struct tcp_stream *tcp, u32 rx_offs, void *buf, int len)
{
	ulong max;

#if IS_ENABLED(CONFIG_CMD_HTTPD_STOCK_RESTORE)
	if (tcp->priv == CONN_STOCK)
		return st_rx(rx_offs, buf, len);
#endif

	/* A GET needs no body; drop whatever follows. */
	if (tcp->priv != CONN_UNKNOWN && tcp->priv != CONN_POST)
		return len;

	/*
	 * The head of every request, wherever its bytes end up: classifying
	 * from this rather than from the staging area is what keeps a request
	 * off a staged image.  Before the refusals below on purpose -- a
	 * segment that is dropped or refused here still came off the wire,
	 * and a stale head is what got a plain GET classified as the POST
	 * that preceded it.
	 */
	if (rx_offs < sizeof(req_head)) {
		u32 n = sizeof(req_head) - rx_offs;

		if (n > (u32)len)
			n = (u32)len;
		memcpy(req_head + rx_offs, buf, n);
	}

	/*
	 * Exactly one connection at a time may stage data at $loadaddr.  The
	 * first one to deliver offset 0 claims it; a segment arriving before
	 * that is refused so it gets retried once the head of the stream is in.
	 */
	if (!up_owner) {
		if (rx_offs != 0) {
			/*
			 * Nothing holds the staging area and this is not
			 * the head of a request, so there is nothing for
			 * these bytes to belong to.  An upload that has
			 * already been answered is the one way a POST
			 * gets here -- take the rest off the wire, the
			 * same thing CONN_DRAIN is for.  Refusing what
			 * will never be accepted leaves the peer
			 * retransmitting into a stream that then never
			 * closes, and this stack serves one at a time.
			 */
			if (tcp->priv == CONN_POST)
				return len;

			return 0;
		}
		up_owner = tcp;
		up_hi = 0;
	} else if (up_owner != tcp) {
		/*
		 * Somebody else's bytes.  0 is "not accepted, send it again",
		 * which is the truth: swallowing them instead told the peer
		 * the request had been read when it had gone nowhere, so a
		 * /ping issued while an upload runs would never be answered
		 * and the page would call the board dead mid-transfer.
		 */
		return 0;
	}

	/*
	 * Already refused.  The 400 is written and waiting; the rest of the
	 * body is taken off the wire and dropped so the browser can finish
	 * sending and get to reading the answer.
	 */
	if (up_failed)
		return len;

	/*
	 * Staged, or being written: the staging area is the image now, and
	 * nothing that gets this far still needs it.  A POST is turned
	 * away before it stages anything -- CONN_POSTBUSY while a write
	 * runs, the page itself while an upload is staged -- so these
	 * bytes are a request that needs no body, or a retransmission of
	 * one whose body is already in.  Take them off the wire and drop
	 * them.
	 *
	 * Below the claim above, and where it sits is the whole point:
	 * that claim is the only one in the file, and
	 * httpd_on_rcv_nxt_update() will not classify a stream that does
	 * not hold it.  Guarding ahead of it instead left every /wr poll
	 * after the first one of a write unclassified, unanswered and
	 * never closed -- with the stack refusing new connections behind
	 * it for as long as that stream took to time out.
	 */
	if (flash_running || up_ready)
		return len;

	/*
	 * The staging area runs from up_base to up_base + upload_max(), and
	 * nothing above that is ours -- U-Boot's own code, heap and stack
	 * start there.  httpd_parse() checks Content-Length against the same
	 * ceiling, but that is a number the client supplied: it can lie, and
	 * it does not exist at all until the headers are in, while these
	 * bytes are being copied from the first segment onwards.  Computed in
	 * 64 bits because rx_offs + len is exactly where a 32-bit sum would
	 * wrap and land back inside the region.
	 */
	max = upload_max();
	if ((u64)rx_offs + (u64)len > (u64)max) {
		httpd_reject("upload runs past the %lu bytes of RAM there is "
			     "room for", max);

		return len;
	}

	memcpy((char *)up_base + rx_offs, buf, len);
	if (rx_offs + len > up_hi)
		up_hi = rx_offs + len;

	return len;
}

/*
 * Which GET this is, from its request line: the page, or one of the JSON /
 * text endpoints.  NULL while the line is still incomplete; anything too
 * long to be one of ours is answered with the page.
 */
static void *httpd_classify_get(const char *req, u32 rx_bytes)
{
	int n = rx_bytes < 320 ? rx_bytes : 320;
	int eol = mem_find(req, n, "\r\n", 2);
	const char *path = req + 4, *sp, *q;
	int plen, qlen = 0;

	if (eol < 0)
		return n < 320 ? NULL : CONN_GET;

	sp = memchr(path, ' ', eol - 4);
	plen = sp ? sp - path : eol - 4;

	/* /dump and /log take arguments; for everything else the path is all. */
	q = memchr(path, '?', plen);
	if (q) {
		qlen = plen - (q - path) - 1;
		plen = q - path;
	}

	if (plen == 5 && !memcmp(path, "/info", 5))
		return CONN_INFO;
	if (plen == 6 && !memcmp(path, "/check", 6))
		return CONN_CHECK;
	if (plen == 5 && !memcmp(path, "/scan", 5)) {
		int k = qlen;

		if (k > (int)sizeof(scan_qs) - 1)
			k = sizeof(scan_qs) - 1;
		if (k > 0)
			memcpy(scan_qs, q + 1, k);
		scan_qs[k > 0 ? k : 0] = '\0';

		return CONN_SCAN;
	}
	if (plen == 4 && !memcmp(path, "/log", 4)) {
		int k = qlen;

		if (k > (int)sizeof(log_qs) - 1)
			k = sizeof(log_qs) - 1;
		if (k > 0)
			memcpy(log_qs, q + 1, k);
		log_qs[k > 0 ? k : 0] = '\0';

		return CONN_LOG;
	}
	if (plen == 3 && !memcmp(path, "/wr", 3)) {
		int k = qlen;

		if (k > (int)sizeof(wr_qs) - 1)
			k = sizeof(wr_qs) - 1;
		if (k > 0)
			memcpy(wr_qs, q + 1, k);
		wr_qs[k > 0 ? k : 0] = '\0';

		return CONN_WR;
	}
	if (plen == 5 && !memcmp(path, "/ping", 5))
		return CONN_PING;
	if (plen == 4 && !memcmp(path, "/net", 4))
		return CONN_NET;
	if (plen == 4 && !memcmp(path, "/env", 4))
		return CONN_ENV;
	if (plen == 9 && !memcmp(path, "/envreset", 9))
		return CONN_ENVRESET;
	if (plen == 7 && !memcmp(path, "/reboot", 7))
		return CONN_REBOOT;
	if (plen == 5 && !memcmp(path, "/boot", 5))
		return CONN_BOOT;
	if (plen == 9 && !memcmp(path, "/bootonce", 9))
		return CONN_BOOTONCE;
	if (plen == 8 && !memcmp(path, "/wipecfg", 8))
		return CONN_WIPECFG;
	if (plen == 7 && !memcmp(path, "/dhcpgw", 7)) {
		int k = qlen;

		if (k > (int)sizeof(dhcpgw_qs) - 1)
			k = sizeof(dhcpgw_qs) - 1;
		if (k > 0)
			memcpy(dhcpgw_qs, q + 1, k);
		dhcpgw_qs[k > 0 ? k : 0] = '\0';

		return CONN_DHCPGW;
	}
	if (plen == 8 && !memcmp(path, "/netmode", 8)) {
		int k = qlen;

		if (k > (int)sizeof(netmode_qs) - 1)
			k = sizeof(netmode_qs) - 1;
		if (k > 0)
			memcpy(netmode_qs, q + 1, k);
		netmode_qs[k > 0 ? k : 0] = '\0';

		return CONN_NETMODE;
	}
	if (plen == 9 && !memcmp(path, "/dumpinfo", 9))
		return CONN_DUMPINFO;
	if (plen == 5 && !memcmp(path, "/dump", 5)) {
		if (qlen > (int)sizeof(dump_qs) - 1)
			qlen = sizeof(dump_qs) - 1;
		if (qlen > 0)
			memcpy(dump_qs, q + 1, qlen);
		dump_qs[qlen > 0 ? qlen : 0] = '\0';

		return CONN_DUMP;
	}

	return CONN_GET;
}

static void httpd_on_rcv_nxt_update(struct tcp_stream *tcp, u32 rx_bytes)
{
	/*
	 * Classify here rather than in rx(): the first segment may be shorter
	 * than the request line, and this callback guarantees that bytes
	 * [0..rx_bytes-1] are present and contiguous.
	 */
	if (tcp->priv == CONN_UNKNOWN) {
		const char *req = req_head;

		if (up_owner != tcp || rx_bytes < 4)
			return;
		if (!memcmp(req, "POST", 4)) {
			/*
			 * Which POST this is sits in the request line, and a
			 * segment boundary can fall anywhere in it -- including
			 * inside the word after the method.  So nothing is
			 * decided until the line is whole, the same way
			 * httpd_classify_get() waits.  Deciding on what had
			 * arrived sent a "POST /stock" whose head came in two
			 * pieces down the multipart path, which then turned it
			 * away as not being one.  A line longer than the 320
			 * bytes read here is nobody's real request; it falls
			 * through to the upload path, where httpd_parse() says
			 * so in as many words.
			 */
			int n = rx_bytes < 320 ? rx_bytes : 320;
			int eol = mem_find(req, n, "\r\n", 2);

			/* Not all of the line is here yet; ask again later. */
			if (eol < 0 && rx_bytes < 320)
				return;

			if (dump_stale()) {
				printf("httpd: the download in flight went quiet, dropping it\n");
				dump_busy = 0;
			}
			if (dump_busy || flash_running) {
				tcp->priv = CONN_POSTBUSY;
				up_owner = NULL;
				return;
			}
			if (up_active) {
				tcp->priv = CONN_GET;	/* one upload at a time */
				up_owner = NULL;
				return;
			}
#if IS_ENABLED(CONFIG_CMD_HTTPD_STOCK_RESTORE)
			/* "POST /stock" -- straight to flash, never staged. */
			if (eol > 11 && !memcmp(req + 4, " /stock", 7) &&
			    (req[11] == '?' || req[11] == ' ')) {
				int r = st_begin(rx_bytes);

				/*
				 * Its head is not all here yet.  Nothing
				 * was claimed or classified, so the next
				 * pass comes back and asks again -- and it
				 * is asked ahead of up_active, or a retry
				 * would meet its own claim and be turned
				 * away as a second upload.
				 */
				if (r > 0)
					return;

				up_active = 1;
				tcp->priv = CONN_STOCK;
				if (!r)
					st_commit(rx_bytes);
				return;
			}
#endif
			up_active = 1;
			tcp->priv = CONN_POST;
		} else {
			void *cls = CONN_GET;

			if (!memcmp(req, "GET ", 4)) {
				cls = httpd_classify_get(req, rx_bytes);
				if (!cls)
					return;	/* request line incomplete */
			}
			tcp->priv = cls;
			up_owner = NULL;	/* release the staging area */

			/*
			 * Only reachable since 0.3.0: the server used to be
			 * silent from the answer to the reset, so nothing could
			 * arrive mid-write.  Two kinds are refused here.
			 *
			 * The first three write the environment volume
			 * underneath a UBI write, move the address out from
			 * under the page that is watching the write, or -- in
			 * the case of /envreset, which runs "env default -a" --
			 * both at once.
			 *
			 * The last two end net_loop(), and the write now lives
			 * inside it: leaving early would strand a half-written
			 * flash.  Refusing rather than deferring is also the
			 * honest answer -- the page is about to offer the
			 * reboot itself, with the read-back result next to it.
			 */
			if (flash_running &&
			    (cls == CONN_ENVRESET || cls == CONN_BOOTONCE ||
			     cls == CONN_NETMODE || cls == CONN_REBOOT ||
			     cls == CONN_BOOT || cls == CONN_WIPECFG ||
			     cls == CONN_DHCPGW)) {
				tcp->priv = CONN_WRBUSY;

				return;
			}

			/* Built now, while the request is still in req_head. */
			if (cls == CONN_INFO)
				info_len = httpd_info();
			else if (cls == CONN_CHECK)
				check_len = httpd_check();
			else if (cls == CONN_SCAN) {
				/*
				 * Same as CONN_DUMP below: a download nobody
				 * is pulling on any more must not make /scan
				 * answer 503 for the rest of the session.
				 */
				if (dump_stale()) {
					printf("httpd: the download in "
					       "flight went quiet, "
					       "dropping it\n");
					dump_busy = 0;
				}
				scan_len = httpd_scan();
			} else if (cls == CONN_LOG)
				log_len = httpd_log();
			else if (cls == CONN_WR)
				wr_len = httpd_wr();
			else if (cls == CONN_PING)
				ping_len = httpd_ping();
			else if (cls == CONN_NET)
				net_len = httpd_net();
			else if (cls == CONN_ENV)
				env_len = httpd_env();
			else if (cls == CONN_ENVRESET)
				envreset_len = httpd_envreset();
			else if (cls == CONN_BOOTONCE)
				bootonce_len = httpd_bootonce();
			else if (cls == CONN_WIPECFG)
				wipecfg_len = httpd_wipecfg();
			else if (cls == CONN_DHCPGW)
				dhcpgw_len = httpd_dhcpgw();
			else if (cls == CONN_NETMODE)
				netmode_len = httpd_netmode();
			else if (cls == CONN_DUMPINFO)
				dumpinfo_len = httpd_dumpinfo();
			else if (cls == CONN_DUMP) {
				if (dump_stale()) {
					printf("httpd: the download in flight went quiet, dropping it\n");
					dump_busy = 0;
				}
				if (dump_busy || flash_running)
					tcp->priv = CONN_DUMPBUSY;
				else
					httpd_dump();
			} else if (cls == CONN_GET)
				/*
				 * A page load means whatever download was in
				 * flight is gone with the old document; without
				 * this an abandoned one would hold the staging
				 * area until the next reboot.
				 */
				dump_busy = 0;
			return;
		}
	}

#if IS_ENABLED(CONFIG_CMD_HTTPD_STOCK_RESTORE)
	if (tcp->priv == CONN_STOCK) {
		st_commit(rx_bytes);
		return;
	}
#endif

	if (tcp->priv != CONN_POST)
		return;

	if (!up_parsed && !up_failed)
		httpd_parse(rx_bytes);

	if (up_parsed && !up_ready && !up_failed && rx_bytes >= up_total)
		httpd_finish();
}

static const char *httpd_response(struct tcp_stream *tcp, int *len)
{
	const char *s;

	if (tcp->priv == CONN_POST) {
		if (up_ready)
			s = resp_ok;
		else if (up_failed)
			s = resp_reject;
		else
			s = NULL;	/* still receiving */
	} else if (tcp->priv == CONN_GET) {
		s = resp_form;
	} else if (tcp->priv == CONN_INFO) {
		*len = info_len;
		return info_buf;
	} else if (tcp->priv == CONN_CHECK) {
		*len = check_len;
		return check_buf;
	} else if (tcp->priv == CONN_LOG) {
		*len = log_len;
		return log_buf;
	} else if (tcp->priv == CONN_WR) {
		*len = wr_len;
		return wr_buf;
	} else if (tcp->priv == CONN_PING) {
		*len = ping_len;
		return ping_buf;
	} else if (tcp->priv == CONN_NET) {
		*len = net_len;
		return net_buf;
	} else if (tcp->priv == CONN_ENV) {
		*len = env_len;
		return env_buf;
	} else if (tcp->priv == CONN_ENVRESET) {
		*len = envreset_len;
		return envreset_buf;
	} else if (tcp->priv == CONN_BOOTONCE) {
		*len = bootonce_len;
		return bootonce_buf;
	} else if (tcp->priv == CONN_WIPECFG) {
		*len = wipecfg_len;
		return wipecfg_buf;
	} else if (tcp->priv == CONN_DHCPGW) {
		*len = dhcpgw_len;
		return dhcpgw_buf;
	} else if (tcp->priv == CONN_NETMODE) {
		*len = netmode_len;
		return netmode_buf;
	} else if (tcp->priv == CONN_BOOT) {
		s = resp_reboot;
	} else if (tcp->priv == CONN_REBOOT) {
		s = resp_reboot;
	} else if (tcp->priv == CONN_WRBUSY) {
		s = resp_wrbusy;
	} else if (tcp->priv == CONN_DUMPBUSY) {
		s = resp_dumpbusy;
	} else if (tcp->priv == CONN_POSTBUSY) {
		s = resp_postbusy;
#if IS_ENABLED(CONFIG_CMD_HTTPD_STOCK_RESTORE)
	} else if (tcp->priv == CONN_STOCK) {
		/* Nothing to say until it is over, either way. */
		*len = st_resp_len;
		return st_resp_len ? st_resp : NULL;
#endif
	} else if (tcp->priv == CONN_SCAN) {
		*len = scan_len;
		return scan_buf;
	} else if (tcp->priv == CONN_DUMPINFO) {
		*len = dumpinfo_len;
		return dumpinfo_buf;
	} else {
		s = NULL;
	}

	*len = s ? strlen(s) : 0;

	return s;
}

static int httpd_tx(struct tcp_stream *tcp, u32 tx_offs, void *buf, int maxlen)
{
	const char *s;
	int total, len;

	if (tcp->priv == CONN_DUMP)
		return dump_tx(tx_offs, buf, maxlen);

	s = httpd_response(tcp, &total);
	if (!s || tx_offs >= (u32)total)
		return 0;

	len = total - tx_offs;
	if (len > maxlen)
		len = maxlen;

	memcpy(buf, s + tx_offs, len);

	return len;
}

static void httpd_on_snd_una_update(struct tcp_stream *tcp, u32 tx_bytes)
{
	const char *s;
	int total;

	if (tcp->priv == CONN_DUMP) {
		if (tx_bytes < (u32)dump_hdr_len + dump_len)
			return;

		/*
		 * Now, not when the read started: the checksum covers the
		 * bytes that actually went out, and the page learning it at
		 * all is proof the transfer ran to the end.
		 */
		if (dump_len && dump_crc_pos == dump_len) {
			dump_last_seq++;
			dump_last_len = dump_len;
			dump_last_crc = dump_crc;
			dump_last_holes = dump_nhole;
			printf("httpd: /dump %s delivered, %u bytes, crc32 0x%08x\n",
			       dump_last_name, dump_len, dump_crc);
		}

		dump_busy = 0;
		tcp_stream_close(tcp);

		return;
	}

	s = httpd_response(tcp, &total);
	if (!s || tx_bytes < (u32)total)
		return;

	/*
	 * Response delivered and acknowledged.  Closing from the tx callback
	 * would break the stream, which is why this lives here.
	 */
	tcp_stream_close(tcp);

#if IS_ENABLED(CONFIG_CMD_HTTPD_STOCK_RESTORE)
	if (tcp->priv == CONN_STOCK) {
		if (st_ok) {
			/* The bootloader this was running from is gone. */
			reboot_pending = 1;
			net_set_state(NETLOOP_SUCCESS);
		} else {
			/* Let them try again without touching the power. */
			printf("httpd: /stock failed, ready for another attempt\n");
			/*
			 * st_rx() swallows the rest while st_failed is
			 * set, and clearing it is what this block is
			 * for; see CONN_DRAIN.  A whole-chip image is
			 * the longest thing this server is ever sent,
			 * so the window between the answer and the last
			 * byte is at its widest right here.
			 */
			tcp->priv = CONN_DRAIN;
			up_active = 0;
			up_owner = NULL;
			st_resp_len = 0;
			st_failed = 0;
		}

		return;
	}
#endif

	/* Same shape as flash_pending: act once the answer is out. */
	if (tcp->priv == CONN_REBOOT) {
		reboot_pending = 1;
		net_set_state(NETLOOP_SUCCESS);

		return;
	}

	if (tcp->priv == CONN_BOOT) {
		boot_pending = 1;
		net_set_state(NETLOOP_SUCCESS);

		return;
	}

	if (tcp->priv == CONN_NETMODE) {
		/* Only now is the answer really out; see netmode_armed. */
		if (netmode_armed) {
			netmode_armed = 0;
			netmode_pending = 1;
			net_set_state(NETLOOP_SUCCESS);
		}

		return;
	}

	if (tcp->priv != CONN_POST)
		return;

	if (up_ready) {
		if (part_find(FIELD_TRYBOOT)) {
			tryboot_pending = 1;
			net_set_state(NETLOOP_SUCCESS);
		} else {
			/*
			 * No NETLOOP_SUCCESS here: httpd_tick() takes the steps
			 * from inside this very net_loop().  Both flags are set
			 * together so that everything guarded on flash_running
			 * is guarded from this instant, not from whenever the
			 * first step happens to run.
			 */
			flash_pending = 1;
			flash_running = 1;
			/*
			 * FS_START clears the log too, but only on the next
			 * tick; the page asks /wr?from=0 the moment this 200
			 * lands, and would read the previous write's "done"
			 * as this one's.
			 */
			wr_reset();
		}
	} else {
		/*
		 * Let the user retry after a failed upload.  The body is
		 * still on its way -- an upload is refused on its headers,
		 * and those arrive first -- while up_failed, the flag that
		 * had httpd_rx() dropping the rest as it came, is cleared
		 * right here.  So the stream is reclassified first and the
		 * bytes keep being dropped.  Without that, every later
		 * segment took the "not accepted, send it again" path:
		 * nothing was acknowledged, the browser retransmitted for
		 * as long as it had patience, and because net/tcp.c keeps
		 * one stream and refuses a new SYN while it is not CLOSED,
		 * nothing else could connect -- not even a reload of the
		 * page.  One refused upload and the board answered nothing
		 * at all until it was restarted.
		 */
		printf("httpd: upload failed, ready for another attempt\n");
		tcp->priv = CONN_DRAIN;
		up_active = 0;
		up_owner = NULL;
		up_parsed = 0;
		up_failed = 0;
		up_body = 0;
		up_total = 0;
		up_bound_len = 0;
	}
}

/*
 * The stack destroys a stream on RST, on running out of retransmit attempts
 * and after rx_inactiv_timeout (30 s) without a packet, and calls this from
 * tcp_stream_destroy() just before wiping it.  Nothing else in this file
 * hears about any of that.
 *
 * It matters because the upload state is claimed by one stream and released
 * by the response to it: a browser that dies mid-upload -- tab closed, cable
 * out, laptop asleep -- left up_active set and up_owner pointing at a stream
 * that no longer exists, and from then on every attempt was turned away with
 * "one upload at a time" until the board was power-cycled.  Which is the one
 * thing a recovery tool must not ask for.
 */
static void httpd_on_closed(struct tcp_stream *tcp)
{
#if IS_ENABLED(CONFIG_CMD_HTTPD_STOCK_RESTORE)
	if (tcp->priv == CONN_STOCK) {
		/*
		 * Finished, but the 200 was never acknowledged -- the browser
		 * crashed or the cable came out right after the last byte.
		 * The reboot is normally queued from on_snd_una_update(), so
		 * without this the board would sit here forever, up_active
		 * set, refusing everything, with a bootloader that has already
		 * been overwritten underneath it.  Nothing left to serve; go.
		 */
		if (st_ok) {
			printf("httpd: /stock done but the answer was never "
			       "acknowledged; rebooting anyway\n");
			reboot_pending = 1;
			net_set_state(NETLOOP_SUCCESS);

			return;
		}
		/* on_snd_una_update() already cleaned up after a refusal the
		 * browser did receive; not ours to undo. */
		if (!up_active)
			return;

		if (st_started && st_done < st_body)
			printf("httpd: /stock connection lost after %u bytes "
			       "were written -- the flash is inconsistent and "
			       "will not boot until a restore is repeated to "
			       "the end\n", st_done);
		else if (st_body && st_done == st_body)
			printf("httpd: /stock connection lost after the image "
			       "was written; the tail erase was dropped and the "
			       "board will boot\n");

		st_hdr_end = st_body = st_done = st_crc = 0;
		st_nskip = st_started = st_failed = st_ok = 0;
		st_wipe = st_wipe_in = st_wipe_n = st_wipe_bad = 0;
		st_resp_len = 0;
		up_active = 0;
		up_owner = NULL;
		printf("httpd: upload connection lost, ready for another "
		       "attempt\n");

		return;
	}
#endif

	/*
	 * /dump is a GET: classification already let go of up_owner, so the
	 * test below returns before dump_busy is ever looked at.  A backup
	 * the browser cancelled would leave the board with its only busy flag
	 * still set, and since dump_stale() is evaluated when the next
	 * request is classified rather than on a clock, what that looks like
	 * from the page is one click refused and the one after it working.
	 * The stream is dead either way; let go of it here.
	 */
	if (tcp->priv == CONN_DUMP && dump_busy) {
		printf("httpd: /dump dropped, ready for another request\n");
		dump_busy = 0;
		dump_wlen = 0;
	}

	if (tcp != up_owner)
		return;

	/*
	 * A finished upload closes through here too -- the 200 is delivered,
	 * on_snd_una_update() queues the work and calls tcp_stream_close().
	 * Undoing the state then would throw away the very thing the page has
	 * already been told is under way, so those two are the signal that
	 * this close is the ordinary end of a job rather than a lost one.
	 */
	if (flash_pending || tryboot_pending)
		return;

	/*
	 * Released first and unconditionally: up_owner is what httpd_rx()
	 * matches every segment against, so a dead stream left in it would
	 * make the server answer 0 -- "send it again" -- to every byte of
	 * every connection after this one.
	 */
	up_owner = NULL;

	/*
	 * A connection that died before it was even classified held nothing
	 * but that claim.  Most of those are a browser probing the port;
	 * there is no upload to report as lost.
	 */
	if (!up_active)
		return;

	up_active = 0;
	up_parsed = 0;
	up_failed = 0;
	up_ready = 0;
	up_body = 0;
	up_total = 0;
	up_bound_len = 0;
	printf("httpd: upload connection lost, ready for another attempt\n");
}

static int httpd_on_create(struct tcp_stream *tcp)
{
	if (tcp->lport != HTTPD_PORT)
		return 0;

	tcp->priv = CONN_UNKNOWN;
	tcp->rx = httpd_rx;
	tcp->tx = httpd_tx;
	tcp->on_closed = httpd_on_closed;
	tcp->on_rcv_nxt_update = httpd_on_rcv_nxt_update;
	tcp->on_snd_una_update = httpd_on_snd_una_update;

	return 1;
}

void httpd_start_server(void)
{
	/*
	 * net_loop() calls this on every entry, and it wipes the state a write
	 * works from (the parts, the stage, the base address).  A write no
	 * longer leaves net_loop() between its steps, so nothing should come
	 * back through here while one is running -- but an env recipe someone
	 * edited could call a network command and do exactly that, and losing
	 * the upload halfway through writing it is not an acceptable way to
	 * find out.  Re-arm the handlers and leave the rest alone.
	 */
	if (flash_running) {
		memset(net_server_ethaddr, 0, 6);
		tcp_stream_set_on_create_handler(httpd_on_create);
		net_set_udp_handler(httpd_dhcp_rx);
		httpd_tick_start();

		return;
	}

	up_active = 0;
	up_owner = NULL;
	up_parsed = 0;
	up_failed = 0;
	up_ready = 0;
	up_body = 0;
	up_total = 0;
	up_nparts = 0;
	up_bound_len = 0;
	flash_pending = 0;
	reboot_pending = 0;
	boot_pending = 0;
	tryboot_pending = 0;
	/*
	 * Both cleared here rather than only in netmode_apply(): a /netmode
	 * whose answer was never acknowledged must not still be armed when
	 * the server comes back up.
	 *
	 * netmode itself is deliberately not re-read from the environment
	 * here.  It is set once per boot by netmode_boot() and then only by
	 * the user; re-loading it on every entry to net_loop() would undo an
	 * unsaved change the moment anything else made the server restart.
	 */
	netmode_pending = 0;
	netmode_armed = 0;
	dump_busy = 0;
	dump_len = 0;
	dump_wlen = 0;
	dump_hdr_len = 0;
	up_base = env_get_hex("loadaddr", CONFIG_SYS_LOAD_ADDR);
	fvols_parse();
	httpd_link_bounce();

	memset(net_server_ethaddr, 0, 6);
	tcp_stream_set_on_create_handler(httpd_on_create);
	net_set_udp_handler(httpd_dhcp_rx);
	httpd_tick_start();

	printf("Airoha Web U-Boot " WEB_VERSION " by " AUTHOR "\n");
	printf("Project " PROJECT_URL "\n");
	printf("Guide   " PORTAL_URL "recovery-guide.html\n");
	/*
	 * The MAC is worth a line of its own: a board that lost its factory
	 * MAC still serves the page just fine, but every browser on the
	 * segment is still talking to whatever ARP learned last time.
	 */
	printf("Using %s device, MAC %pM\n", eth_get_name(), net_ethaddr);
	printf("Listening for HTTP on %pI4 port %d\n", &net_ip, HTTPD_PORT);
	if (netmode == NET_SERVER)
		printf("Handing out DHCP leases from %pI4\n", &net_ip);
	else
		printf("DHCP server off (%s mode)\n", netmode_name(netmode));
	printf("Press Ctrl-C to abort\n");
}

/*
 * ---- flashing ------------------------------------------------------------
 */

/*
 * Move a part down to an aligned address so the NAND layer never sees an
 * awkwardly aligned source.  See FLASH_ALIGN on why stepping back is safe.
 */
static ulong part_align(struct up_part *part)
{
	ulong aligned = part->addr & ~(ulong)(FLASH_ALIGN - 1);

	if (aligned != part->addr && aligned >= up_base) {
		memmove((void *)aligned, (void *)part->addr, part->size);
		part->addr = aligned;
	}

	return part->addr;
}

/* Run $var if the environment defines it, otherwise the built-in fallback. */
static int run_step(const char *var, const char *builtin)
{
	char buf[64];

	if (env_get(var)) {
		snprintf(buf, sizeof(buf), "run %s", var);
		return run_command(buf, 0);
	}

	printf("httpd: $%s not set, using built-in\n", var);

	return run_command(builtin, 0);
}

/*
 * Write one UBI volume in place.  UBI must already be attached; the caller
 * does it once for all volumes.  With create set, a volume that does not
 * exist yet is created for exactly the file, dynamic -- what the board's
 * own scripts do for fit.  No size check against an existing volume: "ubi
 * write" refuses anything larger than the reservation before it touches a
 * byte, and it knows the real figure while this code would only be guessing.
 */
static int flash_vol(ulong addr, const char *name, u32 size, int create)
{
	/*
	 * The create command names the volume twice, and a UBI name runs to
	 * UBIVOL_NAME_MAX - 1 characters: at 160 the command was silently cut
	 * short by snprintf() and "ubi create" would have been handed a
	 * half-written size.
	 */
	char cmd[192];

	if (create) {
		snprintf(cmd, sizeof(cmd),
			 "ubi check %s || ubi create %s 0x%x dynamic",
			 name, name, size);
		if (run_command(cmd, 0)) {
			printf("httpd: creating %s FAILED\n", name);
			return -1;
		}
	}

	printf("httpd: writing %s volume, %u bytes\n", name, size);
	snprintf(cmd, sizeof(cmd), "ubi write 0x%lx %s 0x%x", addr, name, size);
	if (run_command(cmd, 0)) {
		printf("httpd: writing %s FAILED\n", name);
		return -1;
	}

	return 0;
}

#if IS_ENABLED(CONFIG_CMD_HTTPD_STOCK_RESTORE)
/*
 * Raw write to the bare flash device.  Offset 0 is the whole-chip case that
 * puts a factory image back; any other erase-block boundary writes one
 * partition of it.  Either way this goes around the mtd partitions and UBI,
 * and the erase has to cover whole blocks -- "mtd erase" refuses a length
 * that is not a multiple of one.
 */
/*
 * Write an image back at a fixed offset, block by block, position preserving:
 * the source pointer advances over a bad block just as the flash offset does,
 * so byte N of the file always lands at flash offset off + N.
 *
 * This is not what "mtd write" does.  That command skips a bad block without
 * advancing its buffer, which compacts the image -- and a factory dd dump has
 * the bad blocks sitting in it, so everything past the first one would land
 * an eraseblock early and every absolute offset the boot chain relies on
 * would be wrong.
 *
 * mtd_erase() is called directly, and the isbad test above it is the only
 * thing standing between a factory bad block and being erased: nanddev_erase()
 * clears such a block's BBT entry and erases it anyway, which wipes the marker
 * living in its OOB.  The block would then read as good forever after and get
 * trusted with data.  Nothing here may reach mtd_erase() without passing that
 * test first.
 */
static int flash_raw(ulong addr, ulong off, u32 size)
{
	struct mtd_info *m = flash_master();
	const u8 *src = (const u8 *)addr;
	u64 skipped[16];
	int nskip = 0, i;
	u64 pos = off;
	u32 left = size;

	if (!m)
		return -1;

	if (off & (m->erasesize - 1)) {
		printf("httpd: 0x%lx is not on an erase block boundary\n", off);
		return -1;
	}
	if ((u64)off + size > m->size) {
		printf("httpd: 0x%lx + %u bytes runs past the end of %s\n",
		       off, size, m->name);
		return -1;
	}

	printf("httpd: writing %u bytes to %s at 0x%lx, block by block\n",
	       size, m->name, off);

	while (left) {
		u32 n = left < m->erasesize ? left : m->erasesize;
		struct erase_info ei;
		size_t wl = 0;
		int ret;

		if (mtd_block_isbad(m, pos)) {
			if (nskip < (int)ARRAY_SIZE(skipped))
				skipped[nskip] = pos;
			nskip++;
			goto next;
		}

		memset(&ei, 0, sizeof(ei));
		ei.mtd = m;
		ei.addr = pos;
		ei.len = m->erasesize;
		ret = mtd_erase(m, &ei);
		if (ret) {
			printf("httpd: erasing 0x%llx FAILED (%d)\n", pos, ret);
			return -1;
		}

		ret = mtd_write(m, pos, n, &wl, src);
		if (ret || wl != n) {
			printf("httpd: writing 0x%llx FAILED (%d, %u of %u)\n",
			       pos, ret, (u32)wl, n);
			return -1;
		}
next:
		/* Both, always -- that is the whole point. */
		src  += m->erasesize;
		pos  += m->erasesize;
		left -= n;

		if (!((pos - off) & ((32 << 20) - 1)))
			printf("httpd: ... 0x%llx\n", pos);
	}

	if (nskip) {
		printf("httpd: %d bad block(s) left untouched, so what the image "
		       "holds for them was not written:", nskip);
		for (i = 0; i < nskip && i < (int)ARRAY_SIZE(skipped); i++)
			printf(" 0x%llx", skipped[i]);
		printf("%s\n", nskip > (int)ARRAY_SIZE(skipped) ? " ..." : "");
		printf("httpd: everything else landed at its own offset\n");
	}

	return 0;
}
#endif

static int flash_part(const char *what, struct up_part *part, const char *var,
		      const char *builtin)
{
	printf("httpd: writing %s, %u bytes\n", what, part->size);

	env_set_hex("loadaddr", part_align(part));
	env_set_hex("filesize", part->size);

	if (run_step(var, builtin)) {
		printf("httpd: writing %s FAILED\n", what);
		return -1;
	}

	return 0;
}

/*
 * The bootloader is written before the firmware so that a migration which
 * dies halfway still leaves a board that can be recovered over the network.
 */
#define VF_NONE		0
#define VF_VOL		1
#define VF_BL2		2

enum {
	FS_START = 0,
	FS_STOCK,
	FS_ATTACH,
	FS_VOLS,
	FS_BL2,
	FS_UBI,
	FS_FIP,
	FS_FIT,
	FS_VERIFY,
	FS_END,
};

/* What to call this part on the page.  Volume names double as their own. */
static void flash_label(struct up_part *p, char *buf, int n)
{
	struct fvol *v = fvol_find(p->name);

	if (!strcmp(p->name, FIELD_BL2))
		strlcpy(buf, "BL2", n);
	else if (!strcmp(p->name, FIELD_FIP))
		strlcpy(buf, "U-Boot", n);
	else if (!strcmp(p->name, FIELD_FIT))
		strlcpy(buf, "固件", n);
	else if (v)
		strlcpy(buf, v->name, n);
	else if (strcmp(p->name, FIELD_UBIVOL_FILE) ||
		 !part_text(FIELD_UBIVOL_NAME, buf, n))
		strlcpy(buf, p->name, n);
}

/*
 * Where a part ended up, so it can be read back.  Text fields and switches
 * have no target of their own and are skipped.
 */
static int vf_target(struct up_part *p, char *vol, int voln)
{
	struct fvol *v = fvol_find(p->name);

	if (!strcmp(p->name, FIELD_BL2))
		return VF_BL2;

	if (!strcmp(p->name, FIELD_FIP))
		strlcpy(vol, "fip", voln);
	else if (!strcmp(p->name, FIELD_FIT))
		strlcpy(vol, "fit", voln);
	else if (v)
		strlcpy(vol, v->name, voln);
	else if (strcmp(p->name, FIELD_UBIVOL_FILE) ||
		 !part_text(FIELD_UBIVOL_NAME, vol, voln))
		return VF_NONE;

	return VF_VOL;
}

/*
 * The UBI half of vf_read(), spelled out rather than handed to
 * ubi_volume_read().
 *
 * That wrapper prints "Read 4096 bytes from volume fit to ..." on every call,
 * and reading a firmware volume back takes some seven thousand of them: the
 * serial console scrolls for a minute and the recorded log -- the one the
 * diagnostics page shows -- is flushed of everything that came before, which
 * is exactly the output somebody debugging a failed write needs.  It also
 * malloc()s and frees a bounce buffer per call and leaves $filesize set to
 * the size of the last chunk, neither of which is wanted here.
 *
 * Underneath it is this loop, so this is the same read without the noise.
 * check = 0 because the caller is about to CRC the whole thing against what
 * it uploaded, which is a stronger statement than UBI's own per-LEB check and
 * is the entire point of reading it back.
 */
static int vf_ubi_read(const char *name, ulong off, ulong len)
{
	struct ubi_device *ubi = ubi_get_device(0);
	struct ubi_volume *vol;
	u8 *buf = vf_buf;
	int ret = 0;

	if (!ubi)
		return -1;

	vol = ubi_vol_find(ubi, name);
	if (!vol || vol->updating || vol->upd_marker ||
	    (long long)(off + len) > vol->used_bytes) {
		ubi_put_device(ubi);

		return -1;
	}

	while (len) {
		int lnum = off / vol->usable_leb_size;
		int o = off % vol->usable_leb_size;
		int n = vol->usable_leb_size - o;

		if ((ulong)n > len)
			n = len;
		if (ubi_eba_read_leb(ubi, vol, lnum, buf, o, n, 0)) {
			ret = -1;
			break;
		}
		buf += n;
		off += n;
		len -= n;
	}
	ubi_put_device(ubi);

	return ret;
}

/* One window of what was just written, back off the flash. */
static int vf_read(int kind, char *vol, ulong off, ulong len)
{
	if (kind == VF_BL2) {
		struct mtd_info *m = get_mtd_device_nm(BL2_PART);
		size_t rl = 0;
		int ret;

		if (IS_ERR(m))
			return -1;

		ret = mtd_read(m, BL2_IMAGE_OFF + off, len, &rl, vf_buf);
		put_mtd_device(m);

		/* A corrected bit-flip is a read that worked. */
		return (ret && ret != -EUCLEAN) ? -1 : 0;
	}

	return vf_ubi_read(vol, off, len);
}

/*
 * One step per call; FLASH_MORE means "come back", which do_httpd() does by
 * way of net_loop(), so between any two steps the page gets served.
 *
 * Every unit of work is announced in one pass and done in the next.  That is
 * not ceremony: the "s" line has to be on the wire before the board vanishes
 * into a write that answers nothing for ten seconds, or the page would sit
 * there with the previous step still on screen.
 */
static int httpd_flash_step(void)
{
	struct up_part *bl2 = part_find(FIELD_BL2);
	struct up_part *fip = part_find(FIELD_FIP);
	struct up_part *fit = part_find(FIELD_FIT);
	struct up_part *ubifile = part_find(FIELD_UBIVOL_FILE);
	int format = part_find(FIELD_FORMAT) != NULL;
	char name[UBIVOL_NAME_MAX];
	char lab[UBIVOL_NAME_MAX];
	int i;

	switch (flash_stage) {
	case FS_START:
		wr_reset();
		flash_stay = 1;
		flash_said = 0;
		flash_i = 0;
		flash_wrote = 0;
		flash_t0 = get_timer(0);
		httpd_chase_start();
		flash_stage = FS_STOCK;

		return FLASH_MORE;

	case FS_STOCK:
#if IS_ENABLED(CONFIG_CMD_HTTPD_STOCK_RESTORE)
		{
			struct up_part *stock = part_find(FIELD_STOCK);
			char ob[24];
			ulong off = 0;

			/*
			 * Announced and done in one pass, unlike everything
			 * below: the page restores whole images through
			 * POST /stock, which writes as it receives and reports
			 * for itself.  This path is what a curl user gets.
			 */
			if (stock) {
				if (part_text(FIELD_STOCK_OFF, ob, sizeof(ob)))
					off = hextoul(ob, NULL);

				if (flash_raw(part_align(stock), off,
					      stock->size)) {
					wr_printf("f 整片写入失败，详见串口日志\n");

					return FLASH_FAIL;
				}

				/* What this U-Boot ran from may be gone. */
				flash_stay = 0;
				flash_stage = FS_END;

				return FLASH_MORE;
			}
		}
#endif
		flash_stage = FS_ATTACH;

		return FLASH_MORE;

	case FS_ATTACH: {
		int nvols = 0;

		for (i = 0; i < up_nparts; i++)
			if (fvol_find(up_parts[i].name))
				nvols++;

		if (!nvols && !ubifile) {
			flash_stage = FS_BL2;

			return FLASH_MORE;
		}

		/*
		 * Volumes: the factory-data ones from the config, then any
		 * named one -- all in one attach, all in one sitting.  Sizes
		 * were checked before the upload was accepted.
		 */
		if (run_command(CMD_ATTACH_UBI, 0)) {
			printf("httpd: attaching UBI FAILED\n");
			wr_printf("f 挂载 UBI 失败，详见串口日志\n");

			return FLASH_FAIL;
		}
		flash_stage = FS_VOLS;

		return FLASH_MORE;
	}

	case FS_VOLS:
		while (flash_i < up_nparts) {
			struct up_part *p = &up_parts[flash_i];
			struct fvol *v = fvol_find(p->name);

			if (!v) {
				flash_i++;
				continue;
			}
			if (!flash_said) {
				wr_printf("s 写入 %s %u\n", v->name, p->size);
				flash_said = 1;

				return FLASH_MORE;
			}
			flash_said = 0;
			/*
			 * Created when missing, like any other volume here.
			 * A rebuild leaves an empty UBI -- ri and bosa come
			 * back only at the next boot, from _init_env -- and
			 * writing the backup back is exactly what the user
			 * does before that boot.  The exact-size gate in
			 * httpd_validate() is what keeps the created volume
			 * the right size.
			 */
			if (flash_vol(part_align(p), v->name, p->size, 1)) {
				wr_printf("f 写入 %s 卷失败，详见串口日志\n",
					  v->name);

				return FLASH_FAIL;
			}
			flash_wrote += p->size;
			wr_printf("r %s %u %08x\n", v->name, p->size,
				  crc32(0, (const u8 *)part_align(p), p->size));
			flash_i++;

			return FLASH_MORE;
		}

		if (ubifile) {
			/*
			 * httpd_validate() already refused an upload without it,
			 * but on NULL the buffer is left uninitialised -- which
			 * would be a stack string going into a run_command().
			 */
			if (!part_text(FIELD_UBIVOL_NAME, name, sizeof(name))) {
				printf("httpd: the UBI volume name is missing or "
				       "too long\n");
				wr_printf("f 卷名缺失或过长\n");

				return FLASH_FAIL;
			}
			if (!flash_said) {
				wr_printf("s 写入 %s %u\n", name, ubifile->size);
				flash_said = 1;

				return FLASH_MORE;
			}
			flash_said = 0;
			if (flash_vol(part_align(ubifile), name, ubifile->size,
				      1)) {
				wr_printf("f 写入 %s 卷失败，详见串口日志\n", name);

				return FLASH_FAIL;
			}
			flash_wrote += ubifile->size;
			wr_printf("r %s %u %08x\n", name, ubifile->size,
				  crc32(0, (const u8 *)part_align(ubifile),
					ubifile->size));
		}
		flash_i = 0;
		flash_stage = FS_VERIFY;

		return FLASH_MORE;

	case FS_BL2:
		if (!bl2) {
			flash_stage = FS_UBI;

			return FLASH_MORE;
		}
		if (!flash_said) {
			wr_printf("s 写入 BL2 %u\n", bl2->size);
			flash_said = 1;

			return FLASH_MORE;
		}
		flash_said = 0;
		if (flash_part("BL2", bl2, ENV_WRITE_BL2, DEF_WRITE_BL2)) {
			wr_printf("f 写入 BL2 失败，详见串口日志\n");

			return FLASH_FAIL;
		}
		flash_wrote += bl2->size;
		wr_printf("r BL2 %u %08x\n", bl2->size,
			  crc32(0, (const u8 *)part_align(bl2), bl2->size));
		flash_stage = FS_UBI;

		return FLASH_MORE;

	case FS_UBI:
		if (!fip && !fit) {
			flash_i = 0;
			flash_stage = FS_VERIFY;

			return FLASH_MORE;
		}
		if (format && !flash_said) {
			/* The one step with no length to go by; it erases the
			 * whole partition and takes as long as it takes. */
			wr_printf("s 重建 UBI\n");
			flash_said = 1;

			return FLASH_MORE;
		}
		flash_said = 0;
		if (format) {
			printf("httpd: rebuilding UBI\n");
			if (run_step(ENV_FORMAT_UBI, DEF_FORMAT_UBI)) {
				printf("httpd: attaching UBI FAILED\n");
				wr_printf("f 重建 UBI 失败，详见串口日志\n");

				return FLASH_FAIL;
			}
		} else {
			printf("httpd: %s\n", CMD_ATTACH_UBI);
			if (run_command(CMD_ATTACH_UBI, 0)) {
				printf("httpd: attaching UBI FAILED\n");
				wr_printf("f 挂载 UBI 失败，详见串口日志\n");

				return FLASH_FAIL;
			}
		}
		flash_stage = FS_FIP;

		return FLASH_MORE;

	case FS_FIP:
		if (!fip) {
			flash_stage = FS_FIT;

			return FLASH_MORE;
		}
		if (!flash_said) {
			wr_printf("s 写入 U-Boot %u\n", fip->size);
			flash_said = 1;

			return FLASH_MORE;
		}
		flash_said = 0;
		if (flash_part("U-Boot FIP", fip, ENV_WRITE_FIP, DEF_WRITE_FIP)) {
			wr_printf("f 写入 U-Boot 失败，详见串口日志\n");

			return FLASH_FAIL;
		}
		flash_wrote += fip->size;
		wr_printf("r U-Boot %u %08x\n", fip->size,
			  crc32(0, (const u8 *)part_align(fip), fip->size));
		flash_stage = FS_FIT;

		return FLASH_MORE;

	case FS_FIT:
		if (!fit) {
			flash_i = 0;
			flash_stage = FS_VERIFY;

			return FLASH_MORE;
		}
		if (!flash_said) {
			wr_printf("s 写入 固件 %u\n", fit->size);
			flash_said = 1;

			return FLASH_MORE;
		}
		flash_said = 0;
		if (flash_part("firmware", fit, ENV_WRITE_FIT, DEF_WRITE_FIT)) {
			wr_printf("f 写入固件失败，详见串口日志\n");

			return FLASH_FAIL;
		}
		flash_wrote += fit->size;
		wr_printf("r 固件 %u %08x\n", fit->size,
			  crc32(0, (const u8 *)part_align(fit), fit->size));
		flash_i = 0;
		flash_stage = FS_VERIFY;

		return FLASH_MORE;

	/*
	 * Reading it back is this file's own loop, so unlike the writes above
	 * it can stop in the middle and report -- which is why the progress
	 * bar on the page only means something during this half.
	 */
	case FS_VERIFY:
		while (flash_i < up_nparts) {
			struct up_part *p = &up_parts[flash_i];
			int kind = vf_target(p, name, sizeof(name));
			ulong end;

			if (kind == VF_NONE || !p->size) {
				flash_i++;
				continue;
			}
			flash_label(p, lab, sizeof(lab));
			if (!flash_said) {
				wr_printf("s 回读校验 %s %u\n", lab, p->size);
				flash_said = 1;
				vf_off = 0;
				vf_next = 0;
				vf_size = p->size;
				vf_crc = 0;
				vf_src = crc32(0, (const u8 *)part_align(p),
					       p->size);

				return FLASH_MORE;
			}

			end = vf_off + VF_STEP;
			if (end > vf_size)
				end = vf_size;
			while (vf_off < end) {
				ulong n = end - vf_off;

				if (n > VF_CHUNK)
					n = VF_CHUNK;
				if (vf_read(kind, name, vf_off, n)) {
					wr_printf("c bad 读不回 %s\n", lab);

					return FLASH_FAIL;
				}
				vf_crc = crc32(vf_crc, vf_buf, n);
				vf_off += n;
			}

			if (vf_off < vf_size) {
				/* At most twenty lines per part, however big it
				 * is: the log is a fixed buffer. */
				if (vf_off >= vf_next) {
					vf_next = vf_off + vf_size / 20 + 1;
					wr_printf("v %lu %lu\n", vf_off, vf_size);
				}

				return FLASH_MORE;
			}

			wr_printf("v %lu %lu\n", vf_size, vf_size);
			if (vf_crc != vf_src) {
				wr_printf("c bad %s 读回来的内容与上传的不一致\n",
					  lab);

				return FLASH_FAIL;
			}
			flash_said = 0;
			flash_i++;

			return FLASH_MORE;
		}

		wr_printf("c ok\n");
		flash_stage = FS_END;

		return FLASH_MORE;

	case FS_END: {
		ulong secs = (get_timer(flash_t0) + 500) / 1000;

		/* The page keeps this and estimates the next write with it. */
		wr_printf("t %u %lu\n", flash_wrote, secs ? secs : 1);
		wr_printf("done\n");
		break;
	}
	}

	return FLASH_DONE;
}

static int do_httpd(struct cmd_tbl *cmdtp, int flag, int argc,
		    char *const argv[])
{
	int err;

	/*
	 * Before the first net_loop(), which reads ipaddr on the way in.  It
	 * guards itself against the second and later invocations of httpd in
	 * the boot script's loop.
	 */
	netmode_boot();

	/*
	 * Once per command, not once per net_loop(): the loop below re-enters
	 * net_loop() after an address change or a "stay up" flash, and
	 * disarming from httpd_start_server() would have pulled the rug out
	 * from under a /bootonce the user had just set.
	 */
	bootonce_disarm();

	/*
	 * Armed here because this is the first place where both httpd_tick(),
	 * which drives the write, and httpd_flash_step(), which is the write,
	 * are in scope.  This file has no forward declarations.
	 */
	flash_stepper = httpd_flash_step;

	/*
	 * Looping here rather than letting the caller re-enter keeps "stay up"
	 * behaving the same on both ways in: web_uboot_boot_forever would come
	 * back on its own, but check_buttons would fall through to the boot
	 * command instead.
	 */
	for (;;) {
		int staying = 0;

		err = net_loop(HTTPD);

		/* Left with a write still marked running: the last step has
		 * finished and the chase belongs to the cleanup below. */
		if (!flash_running)
			httpd_tick_stop();

		if (err < 0) {
			printf("httpd error: %d\n", err);
			return CMD_RET_FAILURE;
		}

		/*
		 * Before everything else, so that a /reboot or /boot that
		 * turned up mid-write is acted on after the write rather
		 * than instead of it.
		 */
		if (flash_pending) {
			/*
			 * The steps ran inside net_loop(), off httpd_tick();
			 * getting here at all means the last one returned
			 * something other than FLASH_MORE.  Only the cleanup
			 * is left.
			 */
			int r = flash_result;

			/*
			 * Something ended net_loop() with steps still to take.
			 * Nothing should be able to -- every endpoint that sets
			 * NETLOOP_SUCCESS is refused while a write runs -- but
			 * a half-written flash is not what a mistake made later
			 * in this file should cost.  Go back in and finish.
			 */
			if (r == FLASH_MORE)
				continue;

			flash_pending = 0;
			flash_running = 0;
			flash_result = FLASH_MORE;
			flash_stage = FS_START;
			httpd_chase_stop();
			env_set_hex("loadaddr", up_base);

			if (r == FLASH_FAIL)
				printf("httpd: writing FAILED\n");
			else if (!flash_stay)
				break;
			else
				printf("httpd: written; the page decides about "
				       "the reboot\n");

			/*
			 * Staying up either way, and that is the change.  A
			 * write no longer ends in a reset the user did not ask
			 * for: on success the page offers the reboot and says
			 * what landed, and on failure it is still connected to
			 * be told -- which used to be visible on the serial
			 * console and nowhere else.
			 */
			staying = 1;
		}

		if (reboot_pending) {
			printf("httpd: rebooting on request\n");
			run_command("reset", 0);

			return CMD_RET_SUCCESS;
		}

		if (boot_pending) {
			boot_pending = 0;
			printf("httpd: booting the system on request\n");
			run_command("run " BOOT_CMD, 0);
			/* Only here if it did not boot; the caller re-enters. */
			printf("httpd: the system did not boot\n");

			return CMD_RET_SUCCESS;
		}

		if (tryboot_pending) {
			struct up_part *fit = part_find(FIELD_FIT);
			const char *conf = env_get("bootconf");
			char cmd[64];

			tryboot_pending = 0;
			if (fit) {
				/*
				 * The same alignment every flash path gets:
				 * libfdt wants the FIT on an 8-byte boundary
				 * and a multipart part lands wherever the
				 * form data put it.  The configuration name
				 * rides along for the reason every other
				 * boot path carries it: a FIT with no default
				 * configuration does not boot without one,
				 * and the serial TFTP path -- which does
				 * carry it -- would be the only way left in.
				 */
				snprintf(cmd, sizeof(cmd), "bootm 0x%lx%s%s",
					 part_align(fit),
					 conf && *conf ? "#" : "",
					 conf && *conf ? conf : "");
				printf("httpd: booting the upload from "
				       "memory, flash untouched: %s\n", cmd);
				run_command(cmd, 0);
				printf("httpd: that image did not boot; "
				       "the flash was not touched\n");
			}

			/*
			 * Back to the page, not out of httpd.  The page
			 * promises the device returns here when the image
			 * does not boot, and a return keeps that promise only
			 * for web_uboot_boot_forever: check_buttons falls
			 * through to the boot command, so the reset-button
			 * way in would land in the old system with the page
			 * gone -- the only way in for a board with no serial
			 * cable.  Same road as a write that stays up, and
			 * for the same reason.
			 */
			staying = 1;
		}

		/*
		 * Last of the deferred actions.  /netmode is refused outright
		 * while a write is running, so it can no longer be pending
		 * alongside one at all; the ordering is kept because it is
		 * the honest one, not because anything still rests on it.
		 * It used to: httpd_start_server() wiped flash_pending on
		 * every entry, so re-entering net_loop() for an address
		 * change before the write had been dealt with lost an upload
		 * the page had already been told was accepted.  That wipe now
		 * steps around a running write, which is what closed it.
		 *
		 * Not a return either: what follows a returning httpd depends
		 * on how recovery was entered.  web_uboot_boot_forever loops and
		 * would come back, but check_buttons falls through to the boot
		 * command, bootonce runs the saved bootcmd, and the bootmenu
		 * entry lands on an askenv prompt that nobody without a serial
		 * cable can answer.  Changing an address must not depend on any
		 * of that; net_loop() re-reads ipaddr on the way back in.
		 */
		if (netmode_pending)
			netmode_apply();
		else if (!staying)
			return CMD_RET_SUCCESS;

		/* Stopped above, or handed to the cyclic chase for the write
		 * that just ended; either way net_loop() does not bring it
		 * back on its own. */
		httpd_tick_start();
	}

	printf("httpd: flashing done, rebooting\n");
	run_command("reset", 0);

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(
	httpd,	1,	0,	do_httpd,
	"start the web recovery server",
	"\n"
	"    - serve the recovery page on port 80 and hand out one DHCP lease;\n"
	"      uploaded \"" FIELD_BL2 "\", \"" FIELD_FIP "\" and \"" FIELD_FIT "\" fields are\n"
	"      flashed by " ENV_WRITE_BL2 " / " ENV_WRITE_FIP " / \n"
	"      " ENV_WRITE_FIT ", or by a built-in equivalent when those\n"
	"      are not defined"
);
