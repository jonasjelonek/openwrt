# SPDX-License-Identifier: GPL-2.0-only

define Device/netgear-ms510txm
  SOC := rtl9313
  UIMAGE_MAGIC := 0x4E475020
  DEVICE_VENDOR := Netgear
  DEVICE_MODEL := MS510TXM
  DEVICE_PACKAGES := kmod-hwmon-gpiofan kmod-thermal
  IMAGE_SIZE := 31808k
endef
TARGET_DEVICES += netgear-ms510txm
