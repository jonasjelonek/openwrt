DEVICE_VARS += UBOOT

include common.mk

define Build/imx-fit-combined-image
	$(CP) $(IMAGE_KERNEL) $@.boot/openwrt.itb

        cp $@ $@.fs

        $(SCRIPT_DIR)/gen_image_generic.sh $@ \
                $(CONFIG_TARGET_KERNEL_PARTSIZE) \
                $@.boot \
                $(CONFIG_TARGET_ROOTFS_PARTSIZE) \
                $@.fs \
                1024
endef

define Build/imx-usbarmory-sdcard
	$(Build/imx-combined-image-prepare)

	$(Build/imx-fit-combined-image)
	dd if=$(STAGING_DIR_IMAGE)/$(UBOOT)-u-boot-dtb.imx of=$@ bs=1024 seek=1 conv=notrunc

	$(Build/imx-combined-image-clean)
endef

define Device/Default
  PROFILES := Default
  FILESYSTEMS := squashfs ext4
  KERNEL_INSTALL := 1
  KERNEL_SUFFIX := -uImage
  KERNEL_NAME := zImage
  KERNEL := kernel-bin | uImage none
  KERNEL_LOADADDR := 0x80008000
  DTS_DIR := $(DTS_DIR)/nxp/imx
  IMAGES :=
endef

define Device/technexion_imx7d-pico-pi
  DEVICE_VENDOR := TechNexion
  DEVICE_MODEL := PICO-PI-IMX7D
  UBOOT := pico-pi-imx7d
  DEVICE_DTS := imx7d-pico-pi
  DEVICE_PACKAGES := kmod-sound-core kmod-sound-soc-imx kmod-sound-soc-imx-sgtl5000 \
	kmod-can kmod-can-flexcan kmod-can-raw kmod-leds-gpio \
	kmod-input-touchscreen-edt-ft5x06 kmod-usb-hid kmod-btsdio \
	kmod-brcmfmac brcmfmac-firmware-4339-sdio cypress-nvram-4339-sdio
  FILESYSTEMS := squashfs
  IMAGES := combined.bin sysupgrade.bin
  IMAGE/combined.bin := append-rootfs | pad-extra 128k | imx-sdcard-raw-uboot
  IMAGE/sysupgrade.bin := sysupgrade-tar | append-metadata
endef
TARGET_DEVICES += technexion_imx7d-pico-pi

define Device/withsecure_usbarmory-mk2
  DEVICE_VENDOR := WithSecure
  DEVICE_MODEL := USB armory Mk II
  UBOOT := usbarmory-mark-two
  DEVICE_DTS := imx6ulz-usbarmory-mk2
  DEVICE_PACKAGES := kmod-crypto-hw-mxs-dcp kmod-usb-gadget-eth kmod-usb-gadget-ncm \
	kmod-usb-gadget-serial kmod-usb-gadget-cdc-composite \
	kmod-usb-net-cdc-ether kmod-usb-net-cdc-ncm usbgadget
  FILESYSTEMS := squashfs ext4
  SUPPORTED_DEVICES += withsecure,imx6ulz-usbarmory-mk2
  KERNEL := kernel-bin | lzma | fit lzma $$(DTS_DIR)/$$(DEVICE_DTS).dtb
  IMAGES := combined.bin sysupgrade.bin
  IMAGE/combined.bin := append-rootfs | pad-extra 128k | imx-usbarmory-sdcard
  IMAGE/sysupgrade.bin := sysupgrade-tar | append-metadata
endef
TARGET_DEVICES += withsecure_usbarmory-mk2
