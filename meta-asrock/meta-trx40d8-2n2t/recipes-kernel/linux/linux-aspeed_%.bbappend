FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI:append:trx40d8-2n2t = " \
    file://0001-arm-dts-aspeed-build-trx40d8-2n2t-dtb.patch \
    file://0002-usb-gadget-rndis-add-cdc-ethernet-descriptor.patch \
    file://0003-usb-gadget-rndis-add-initial-packet-filter.patch \
    file://0004-spi-aspeed-support-manual-host-flash-binding.patch \
    file://0005-spi-aspeed-initialize-host-flash-controller.patch \
    file://trx40d8-2n2t.cfg \
"
