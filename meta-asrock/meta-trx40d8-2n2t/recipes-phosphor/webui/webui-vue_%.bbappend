FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append:trx40d8-2n2t = " \
    file://0001-webui-vue-add-bios-configuration-page.patch \
    "

# The source patch changes the otherwise allarch WebUI for this machine.
PACKAGE_ARCH:trx40d8-2n2t = "${MACHINE_ARCH}"

python allarch_package_arch_handler:trx40d8-2n2t() {
    return
}
