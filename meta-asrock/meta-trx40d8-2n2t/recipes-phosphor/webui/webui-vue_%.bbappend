FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append:trx40d8-2n2t = " \
    file://0001-webui-vue-add-bios-configuration-page.patch \
    file://0002-webui-vue-keep-direct-virtual-media-slot.patch \
    file://0003-virtual-media-browse-images-on-SMB-and-NFS-shares.patch \
    file://0004-webui-vue-add-TR1-fan-curve-editor.patch \
    "

# The source patch changes the otherwise allarch WebUI for this machine.
PACKAGE_ARCH:trx40d8-2n2t = "${MACHINE_ARCH}"

python allarch_package_arch_handler:trx40d8-2n2t() {
    return
}

VITE_VIRTUAL_MEDIA_LIST_ENABLED:trx40d8-2n2t = "true"
export VITE_VIRTUAL_MEDIA_LIST_ENABLED
