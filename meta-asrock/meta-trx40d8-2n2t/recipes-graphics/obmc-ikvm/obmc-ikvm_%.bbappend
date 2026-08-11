FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append:trx40d8-2n2t = " file://10-pin-hid-udc.conf"

do_install:append:trx40d8-2n2t() {
    install -d ${D}${systemd_system_unitdir}/obmc-ikvm.service.d
    install -m 0644 ${UNPACKDIR}/10-pin-hid-udc.conf \
        ${D}${systemd_system_unitdir}/obmc-ikvm.service.d/
}

FILES:${PN}:append:trx40d8-2n2t = " \
    ${systemd_system_unitdir}/obmc-ikvm.service.d/10-pin-hid-udc.conf \
"
