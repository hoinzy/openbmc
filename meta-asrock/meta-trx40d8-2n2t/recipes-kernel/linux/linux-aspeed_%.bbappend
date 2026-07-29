FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI:append:trx40d8-2n2t = " \
    file://0001-arm-dts-aspeed-build-trx40d8-2n2t-dtb.patch \
    file://trx40d8-2n2t.cfg \
"
