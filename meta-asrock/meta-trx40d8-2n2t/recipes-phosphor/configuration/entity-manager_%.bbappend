FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append:trx40d8-2n2t = " \
    file://0001-entity-manager-reset-exposes-json-pointer.patch \
    file://trx40d8-2n2t.json \
    "

do_install:append:trx40d8-2n2t() {
    install -d ${D}${datadir}/entity-manager/configurations/asrock
    install -m 0644 ${UNPACKDIR}/trx40d8-2n2t.json \
        ${D}${datadir}/entity-manager/configurations/asrock/
}

FILES:${PN}:append:trx40d8-2n2t = " \
    ${datadir}/entity-manager/configurations/asrock/trx40d8-2n2t.json \
"
