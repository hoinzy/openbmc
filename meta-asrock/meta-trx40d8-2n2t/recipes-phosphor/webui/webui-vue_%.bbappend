FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append:trx40d8-2n2t = " \
    file://0001-webui-vue-add-bios-configuration-page.patch \
    "
