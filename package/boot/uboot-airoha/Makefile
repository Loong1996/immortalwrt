include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_VERSION:=2026.07
PKG_HASH:=78e8bfc382fe388f9b55aa1daf8c563522a037779b5d4c349d1415e381f1243e
PKG_BUILD_DEPENDS:=arm-trusted-firmware-tools/host

UBOOT_USE_INTREE_DTC:=1

include $(INCLUDE_DIR)/u-boot.mk
include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/host-build.mk

# What BL2 will load as BL33 (files/fip/check-bl33.sh): LZMA-packed, into
# a 0x58000 buffer that also holds ~16 KiB of decoder state, and unpacked
# into 1 MiB.  Boards whose BL2 was built from the same ATF with wider limits
# say so; the SPI NAND BL2s already in the field have these.
define U-Boot/Default
  BUILD_TARGET:=airoha
  LZMA_COMPRESS:=1
  HIDDEN:=1
  BL33_MAX:=0x100000
  BL33_LZMA_MAX:=0x54000
endef

define U-Boot/an7581_fiberhome_hg5382a
  NAME:=FiberHome HG5382A
  UBOOT_CONFIG:=an7581_fiberhome_hg5382a
  BUILD_DEVICES:=fiberhome_hg5382a-ubi
  BUILD_SUBTARGET:=an7581
  BL2_IMAGE:=an7581-pnand-bl2.bin
  BL31_IMAGE:=an7581-bl31.bin
  # The pnand BL2 unpacks up to 2 MiB from a 0x80000 buffer (ATF patch
  # 102).  The FIP must also fit the 0x7f800 BL2 takes over XMODEM, next to
  # BL31, hence 0x70000 rather than 0x7c000.
  BL33_MAX:=0x200000
  BL33_LZMA_MAX:=0x70000
endef

# Locked vendor bootloader: no BL2 or FIP, the vendor U-Boot boots this one
# out of the chainloader partition.  CHAINLOAD_SLOT builds that partition's
# image here, with the U-Boot tree's own mkimage; see files/chainload/.
define U-Boot/an7581_gemtek_xr1710g
  NAME:=Gemtek XR1710G
  UBOOT_CONFIG:=an7581_gemtek_xr1710g
  BUILD_DEVICES:=gemtek_xr1710g-ubi
  BUILD_SUBTARGET:=an7581
  CHAINLOAD_SLOT:=1
endef

define U-Boot/an7581_gemtek_w1700k
  NAME:=Gemtek W1700K (UBI)
  UBOOT_CONFIG:=an7581_w1700k
  BUILD_DEVICES:=gemtek_w1700k-ubi
  BUILD_SUBTARGET:=an7581
endef

define U-Boot/an7581_nokia_valyrian
  NAME:=Nokia Valyrian
  UBOOT_CONFIG:=an7581_nokia_valyrian
  BUILD_DEVICES:=nokia_valyrian
  BUILD_SUBTARGET:=an7581
  BL2_IMAGE:=an7581-bl2.bin
  BL31_IMAGE:=an7581-bl31.bin
endef

define U-Boot/an7581_nokia_xg-040g-md
  NAME:=Nokia XG-040G-MD
  UBOOT_CONFIG:=an7581_nokia_xg-040g-md
  BUILD_DEVICES:=nokia_xg-040g-md-ubi
  BUILD_SUBTARGET:=an7581
  BL2_IMAGE:=an7581-bl2.bin
  BL31_IMAGE:=an7581-bl31.bin
endef

define U-Boot/an7581_nokia_xg-040g-tf
  NAME:=Nokia XG-040G-TF
  UBOOT_CONFIG:=an7581_nokia_xg-040g-tf
  BUILD_DEVICES:=nokia_xg-040g-tf-ubi
  BUILD_SUBTARGET:=an7581
  BL2_IMAGE:=an7581-bl2.bin
  BL31_IMAGE:=an7581-bl31.bin
endef

define U-Boot/an7581_znxt_zn504xg-d
  NAME:=ZNXT ZN504XG-D
  UBOOT_CONFIG:=an7581_znxt_zn504xg-d
  BUILD_DEVICES:=znxt_zn504xg-d-ubi
  BUILD_SUBTARGET:=an7581
  BL2_IMAGE:=an7581-bl2.bin
  BL31_IMAGE:=an7581-bl31.bin
endef

define U-Boot/an7581_rfb
  NAME:=AN7581 Reference Board
  UBOOT_CONFIG:=an7581_evb
  BUILD_DEVICES:=airoha_an7581-evb airoha_an7581-evb-emmc-eagle airoha_an7581-evb-emmc-kite
  BUILD_SUBTARGET:=an7581
  BL2_IMAGE:=an7581-bl2.bin
  BL31_IMAGE:=an7581-bl31.bin
  DEPENDS:=+trusted-firmware-a-an7581-bl31
endef

define U-Boot/an7583_nokia_xg-040g-mf
  NAME:=Nokia XG-040G-MF
  UBOOT_CONFIG:=an7583_nokia_xg-040g-mf
  BUILD_DEVICES:=nokia_xg-040g-mf-ubi
  BUILD_SUBTARGET:=an7583
  BL2_IMAGE:=an7583-bl2.bin
  BL31_IMAGE:=an7583-bl31.bin
  DEPENDS:=+trusted-firmware-a-an7583-bl31
endef

define U-Boot/an7583_rfb
  NAME:=AN7583 Reference Board
  UBOOT_CONFIG:=an7583_evb
  BUILD_DEVICES:=airoha_an7583-evb airoha_an7583-evb-emmc
  BUILD_SUBTARGET:=an7583
  BL2_IMAGE:=an7583-bl2.bin
  BL31_IMAGE:=an7583-bl31.bin
  DEPENDS:=+trusted-firmware-a-an7583-bl31
endef

define U-Boot/en7523_rfb
  NAME:=EN7523 Reference Board
  UBOOT_CONFIG:=en7523_evb
  BUILD_DEVICES:=airoha_en7523-evb
  BUILD_SUBTARGET:=en7523
  BL2_IMAGE:=en7523-bl2.bin
  BL31_IMAGE:=en7523-bl31.bin
endef

UBOOT_TARGETS := \
	an7581_fiberhome_hg5382a \
	an7581_gemtek_w1700k \
	an7581_gemtek_xr1710g \
	an7581_nokia_valyrian \
	an7581_nokia_xg-040g-md \
	an7581_nokia_xg-040g-tf \
	an7581_znxt_zn504xg-d \
	an7581_rfb \
	an7583_nokia_xg-040g-mf \
	an7583_rfb \
	en7523_rfb

UBOOT_CUSTOMIZE_CONFIG := \
	--disable TOOLS_KWBIMAGE \
	--disable TOOLS_LIBCRYPTO \
	--disable TOOLS_MKEFICAPSULE \
	--enable SERIAL_RX_BUFFER \
	--set-val SERIAL_RX_BUFFER_SIZE 256

# src/ is copied and patches apply first. The web-recovery boards
# then get their defconfig and env from files/web-uboot/, so the version
# string and the shared menu live in one place. Other boards are untouched.
define Build/Prepare
	$(call Build/Prepare/Default)
	python3 ./files/web-uboot/assemble.py \
		--tree $(PKG_BUILD_DIR) --dest $(PKG_BUILD_DIR)
endef

define Build/Configure
	$(call Build/Configure/U-Boot)
	sed -i 's/CONFIG_TOOLS_LIBCRYPTO=y/# CONFIG_TOOLS_LIBCRYPTO is not set/' $(PKG_BUILD_DIR)/.config
endef

# A board with web recovery (CONFIG_CMD_HTTPD) must name its LED chase with
# httpd-chase-leds in /options/u-boot, or the panel stays dark while it waits.
# The built control DTB is checked, so where the DTS lives does not matter.
define Build/Compile
	$(call Build/Compile/U-Boot)
	if grep -q '^CONFIG_CMD_HTTPD=y' $(PKG_BUILD_DIR)/.config; then \
		[ -f $(PKG_BUILD_DIR)/dts/dt.dtb ] || { \
			echo "$(BUILD_VARIANT): dts/dt.dtb not found" >&2; exit 1; }; \
		grep -qa 'httpd-chase-leds' $(PKG_BUILD_DIR)/dts/dt.dtb || { \
			echo "$(BUILD_VARIANT): CONFIG_CMD_HTTPD=y but its U-Boot DTS" \
			     "declares no httpd-chase-leds in /options/u-boot" >&2; \
			exit 1; }; \
	fi
	$(if $(LZMA_COMPRESS), $(STAGING_DIR_HOST)/bin/lzma e \
		$(PKG_BUILD_DIR)/u-boot.bin \
		$(PKG_BUILD_DIR)/u-boot.lzma)
	$(if $(and $(BL2_IMAGE),$(LZMA_COMPRESS)), \
		$(SHELL) ./files/fip/check-bl33.sh $(BUILD_VARIANT) \
			$(PKG_BUILD_DIR)/u-boot.bin $(PKG_BUILD_DIR)/u-boot.lzma \
			$(BL33_MAX) $(BL33_LZMA_MAX))
	$(if $(CHAINLOAD_SLOT), \
		XZ=$(STAGING_DIR_HOST)/bin/xz $(SHELL) ./files/chainload/build.sh \
			$(TARGET_CROSS) $(PKG_BUILD_DIR) $(PKG_BUILD_DIR)/u-boot.lzma \
			"$(NAME) Web U-Boot" $(PKG_BUILD_DIR)/chainloader-slot.bin)
endef

# don't stage files to bindir, let target/linux/airoha/image/*.mk do that
define Package/u-boot/install
endef

define Build/InstallDev
	$(INSTALL_DIR) $(STAGING_DIR_IMAGE)
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/u-boot.$(if $(LZMA_COMPRESS),lzma,bin) \
		$(STAGING_DIR_IMAGE)/$(BUILD_VARIANT)-u-boot.$(if $(LZMA_COMPRESS),lzma,bin)
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/u-boot.dtb \
		$(STAGING_DIR_IMAGE)/$(BUILD_VARIANT)-u-boot.dtb
	$(if $(CHAINLOAD_SLOT), $(INSTALL_DATA) $(PKG_BUILD_DIR)/chainloader-slot.bin \
		$(STAGING_DIR_IMAGE)/$(BUILD_VARIANT)-chainloader-slot.bin)
endef

$(eval $(call BuildPackage/U-Boot))
