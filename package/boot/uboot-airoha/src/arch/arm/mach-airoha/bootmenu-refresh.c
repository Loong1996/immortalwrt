// SPDX-License-Identifier: GPL-2.0
/*
 * Keeping the saved environment's boot menu in step with the firmware.
 *
 * Shared by every airoha SoC: the logic gates itself on a web_uboot_envver in the
 * board's default environment, so a board that does not define one returns
 * early and nothing happens.  Lives here rather than in one board file so
 * that a second board does not have to carry a copy.
 */

#include <env.h>
#include <event.h>
#include <search.h>
#include <stdio.h>
#include <linux/kernel.h>
#include <linux/string.h>

/*
 * A new FIP ships a new default environment, but the saved one shadows it
 * completely: ubootenv holds a full copy, so menu entries added since the
 * board was first initialised never appear, and the title goes on advertising
 * the version it was installed with.
 *
 * The blunt answer -- "Reset all settings to factory defaults" -- also clears
 * ethaddr, and on a board whose ri volume was rebuilt that is the only copy of
 * the MAC left.  So instead the default environment carries a version, and
 * this re-imports just the variables that spell out the menu whenever the
 * saved copy is behind.
 *
 * Everything the running system owns is deliberately left out of that list:
 * bootdelay and bootmenu_delay (raised from 0 to 3 by _switch_to_menu),
 * bootmenu_0 (swapped for bootmenu_0d once the board is initialised), and
 * ethaddr, which is not in the default environment at all.
 *
 * A board that was never initialised has no saved environment to be behind:
 * env_get() is already answering out of the compiled-in default, so the
 * versions match and nothing happens.  Other an7581 boards have no version in
 * their default environment and return early.
 */
/*
 * ethaddr_factory reads six bytes out of the ri volume.  On a board that went
 * through "rebuild UBI" that volume was recreated empty, so what it reads is
 * an erased NAND page and what lands in ethaddr is ff:ff:ff:ff:ff:ff.
 *
 * eth-uclass only tests the environment MAC with is_zero_ethaddr(), never
 * is_valid_ethaddr(), so the broadcast address is taken at face value and
 * then used as a source address -- DHCP still works, being broadcast anyway,
 * while every unicast reply is dropped by the peer.  The env script now
 * guards against this, but it only runs from _firstboot, so a board already
 * carrying the bad value in its saved environment never meets the fix.
 *
 * EVT_SETTINGS_R fires after initr_env() and before initr_net(), which is the
 * one window where clearing it still changes what the NIC comes up with.
 */
static int ethaddr_dropped;

static int drop_broken_ethaddr(void)
{
	const char *mac = env_get("ethaddr");

	if (!mac || strcmp(mac, "ff:ff:ff:ff:ff:ff"))
		return 0;

	printf("ethaddr is ff:ff:ff:ff:ff:ff -- an erased ri volume read as a MAC.\n");
	printf("Dropping it; a generated address will be used and saved.\n");
	printf("To put the real one back: setenv ethaddr <MAC on the label> ; saveenv\n");

	env_set("ethaddr", NULL);
	ethaddr_dropped = 1;

	return 0;
}
EVENT_SPY_SIMPLE(EVT_SETTINGS_R, drop_broken_ethaddr);

#define ENVVER	"web_uboot_envver"

/*
 * What a saved environment gets re-imported from this build's defaults once
 * it turns out to be behind.  Everything the running system owns is
 * deliberately absent: bootdelay and bootmenu_delay (raised 0 -> 3 by
 * _switch_to_menu), bootmenu_0 (swapped for bootmenu_0d on init), ethaddr,
 * and web_uboot_netmode and its two companions, which are the user's own
 * network choice rather than anything this build ships.
 *
 * The web_uboot_write_* scripts are here because they are defaults: they
 * exist so that the flashing steps can be inspected and overridden from the
 * console, and a board upgrading across the rename would otherwise be left
 * with only the old httpd_* names.  Harmless in itself -- net/httpd.c falls
 * back to its built-in copies -- but the override point would have quietly
 * gone missing.
 */
static char * const refresh_vars[] = {
	ENVVER,
	"bootmenu_title",
	"bootmenu_1", "bootmenu_2", "bootmenu_3", "bootmenu_4", "bootmenu_5",
	"bootmenu_6", "bootmenu_7", "bootmenu_8", "bootmenu_9",
	"web_uboot_show_about",
	"web_uboot_write_bl2", "web_uboot_write_fip", "web_uboot_format_ubi",
	/*
	 * The automatic ways back into recovery.  Left out at first, which
	 * made the version bump a no-op for them: an upgraded board picked
	 * up the new menu entry but kept booting to TFTP on failure and on
	 * the reset button -- the two paths a user without a serial cable
	 * actually has.  Firmware defaults, not user settings, so they
	 * belong here; bootdelay, bootmenu_0 and ethaddr do not.
	 */
	"boot_ubi", "web_uboot_boot_forever", "check_buttons",
	/* The same for a chainloaded board (chainload.env, policy-gemtek.env). */
	"web_uboot_write_chain", "web_uboot_boot_hook",
	"web_uboot_luci_trigger", "web_uboot_dsd_sync",
	/*
	 * The TFTP file names.  They are not settings either: assemble.py
	 * builds all four out of the image profile name and refuses to let
	 * an env fragment spell them out, so they change whenever a profile
	 * is renamed -- as znxt_zn504xg-d did on its way to -ubi.  Without
	 * them here the saved copy keeps the names of the build it was
	 * installed with, and menu entries 1, 3, 4 and 5 go on asking the
	 * TFTP server for files that are no longer built under that name.
	 */
	"bootfile", "bootfile_bl2", "bootfile_fip", "bootfile_upg",
	"bootfile_chain",
};

/*
 * A chainloaded board does not start from a clean slate: the U-Boot that
 * sat in its chainloader partition before this one used the same ubootenv
 * volumes, and left its own environment in them -- on the XR1710G another
 * project's recovery U-Boot, whose bootcmd calls a command this one does not
 * have.  Refreshing the menu variables on top of that would leave a hybrid
 * that runs half of each.
 *
 * So a board that says so (web_uboot_foreign_env=reset in its default
 * environment) starts over from its defaults when the saved environment was
 * never written by any version of this U-Boot -- no web_uboot_envver, nor
 * envver, the name it had before.  What the old one knew about this
 * particular unit is carried across: its MACs, and a one-shot request for
 * recovery that the firmware may have just set.  So is what this boot has
 * set up by now and would not set again -- the version string the menu
 * title is built from, the control FDT address, the console assignment.
 *
 * Done here, after preboot, like the refresh below: these boards build
 * without CONFIG_USE_PREBOOT, so an old environment's preboot never runs
 * and has nothing to do before this.
 */
static char * const foreign_keep[] = {
	"ethaddr", "eth1addr", "recovery_trigger",
	"ver", "fdtcontroladdr", "stdin", "stdout", "stderr",
};

static int adopt_foreign_env(void)
{
	char val[ARRAY_SIZE(foreign_keep)][128];
	char want[16];
	const char *v;
	int i;

	if (env_get_default_into("web_uboot_foreign_env", want, sizeof(want)) < 0 ||
	    strcmp(want, "reset"))
		return 0;
	if (env_get(ENVVER) || env_get("envver"))
		return 0;

	for (i = 0; i < ARRAY_SIZE(foreign_keep); i++) {
		v = env_get(foreign_keep[i]);
		strlcpy(val[i], v ? v : "", sizeof(val[i]));
	}

	printf("Saved environment was not written by this U-Boot; starting over from its defaults\n");
	env_set_default(NULL, 0);

	for (i = 0; i < ARRAY_SIZE(foreign_keep); i++)
		if (val[i][0])
			env_set(foreign_keep[i], val[i]);

	return 1;
}

/*
 * _bootmenu_update_title is what appends $ver to the title, and it deletes
 * itself the first time it runs -- it exists for the one-time setup of a
 * fresh environment.  So after re-importing bootmenu_title below there is
 * nothing left to put the version back, and the menu would go on showing no
 * version at all.  Do it here instead.
 *
 * The two never double up: a rebuilt environment runs the script and matches
 * the version (so this does not fire), while a firmware upgrade fires this
 * script is long gone.
 */
static void append_version_to_title(void)
{
	const char *title = env_get("bootmenu_title");
	const char *ver = env_get("ver");
	char buf[256];

	if (!title || !ver)
		return;

	snprintf(buf, sizeof(buf), "%s       \033[33m%s\033[0m", title, ver);
	env_set("bootmenu_title", buf);
}

static int refresh_boot_menu(void)
{
	char want[16];
	const char *have;
	int dirty = ethaddr_dropped | adopt_foreign_env();

	if (env_get_default_into(ENVVER, want, sizeof(want)) >= 0 && want[0]) {
		have = env_get(ENVVER);
		if (!have || strcmp(have, want)) {
			printf("Boot menu: refreshing saved environment for v%s\n",
			       want);
			env_set_default_vars(ARRAY_SIZE(refresh_vars),
					     refresh_vars,
					     H_INTERACTIVE);
			append_version_to_title();
			dirty = 1;
		}
	}

	if (!dirty)
		return 0;

	/*
	 * By now eth-uclass has replaced a dropped ethaddr with a generated
	 * one; saving is what stops it being a different address on every
	 * boot, which would leave every peer holding a stale ARP entry.
	 *
	 * Saving is best-effort on purpose.  A first-time migration reaches
	 * here before _init_env has created the env volumes, and it does not
	 * need the write anyway -- it is already running off the defaults.
	 */
	if (env_save())
		printf("Environment not saved, changes apply to this boot only\n");

	return 0;
}
EVENT_SPY_SIMPLE(EVT_POST_PREBOOT, refresh_boot_menu);

