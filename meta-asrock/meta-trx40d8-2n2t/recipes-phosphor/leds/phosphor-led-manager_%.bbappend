FILESEXTRAPATHS:prepend:trx40d8-2n2t := "${THISDIR}/${PN}:"

SRC_URI:append:trx40d8-2n2t = " file://led-group-config.json"

do_install:append:trx40d8-2n2t() {
    install -m 0644 ${UNPACKDIR}/led-group-config.json \
        ${D}${datadir}/phosphor-led-manager/
}
