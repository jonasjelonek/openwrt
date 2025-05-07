# SPDX-License-Identifier: GPL-2.0-only

define Device/plasmacloud-common
  SOC := rtl9312
  UIMAGE_MAGIC := 0x93100000
  DEVICE_VENDOR := Plasma Cloud
  IMAGE_SIZE := 13376k
  BLOCKSIZE := 64k
  DEVICE_PACKAGES := kmod-hwmon-adt7475
  KERNEL_INITRAMFS := \
    kernel-bin | \
    append-dtb | \
    lzma | \
    uImage lzma
  KERNEL := kernel-bin | append-dtb | lzma | uImage lzma | pad-to $$(BLOCKSIZE)
  IMAGES += factory.bin
  IMAGE/factory.bin := append-kernel | append-rootfs | pad-rootfs | check-size
  IMAGE/sysupgrade.bin := append-rootfs | pad-rootfs | sysupgrade-tar rootfs=$$$$@ | append-metadata
endef

define Device/plasmacloud_esx28
  $(Device/plasmacloud-common)
  DEVICE_MODEL := ESX28
endef
TARGET_DEVICES += plasmacloud_esx28

define Device/plasmacloud_psx28
  $(Device/plasmacloud-common)
  DEVICE_MODEL := PSX28
  DEVICE_PACKAGES += poemgr
endef
TARGET_DEVICES += plasmacloud_psx28

define Device/zyxel_xs1930-10
  SOC := rtl9313
#  UIMAGE_MAGIC := 0x93100007
  UIMAGE_MAGIC := 0x4E475020
#  ZYXEL_VERS := ABQE
  DEVICE_VENDOR := Zyxel
  DEVICE_MODEL := XS1930-10
  DEVICE_PACKAGES := kmod-hwmon-gpiofan kmod-thermal
  IMAGE_SIZE := 31808k
#  KERNEL_LOADADDR := 0x81000000
#  KERNEL_INITRAMFS := \
#        kernel-bin | \
#        append-dtb | \
#        gzip | \
#        zyxel-vers | \
#        uImage gzip
endef
TARGET_DEVICES += zyxel_xs1930-10

