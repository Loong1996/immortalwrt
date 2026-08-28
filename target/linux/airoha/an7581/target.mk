ARCH:=aarch64
SUBTARGET:=an7581
BOARDNAME:=AN7581 / AN7566 / AN7551
CPU_TYPE:=cortex-a53
KERNELNAME:=Image dtbs
FEATURES+=pwm

# uboot-envtools 不放在这里：tcboot 变体必须不带它（fw_setenv 在 env 分区 CRC
# 无效时会把自己编译进去的通用默认环境写进 flash，当场无感，重启才失联），而
# DEFAULT_PACKAGES 里的包只能靠 DEVICE_PACKAGES 的 -pkg negation 去掉 —— 那条
# 路径最终落到 image.mk 里带 `-` 前缀的 apk del，删不掉也不会让构建失败。改为
# 由各设备在 DEVICE_PACKAGES 里各自声明。
DEFAULT_PACKAGES += \
	airoha-en7581-npu-firmware

define Target/Description
	Build firmware images for Airoha an7581 ARM based boards.
endef

