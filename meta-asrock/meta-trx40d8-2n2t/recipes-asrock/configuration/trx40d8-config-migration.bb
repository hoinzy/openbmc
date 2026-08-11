SUMMARY = "Migrate persistent TRX40D8 Entity Manager configuration"
DESCRIPTION = "Refreshes the board configuration across image upgrades while preserving user fan settings"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://trx40d8-config-migration \
    file://trx40d8-config-migration.service \
    file://config-schema-version \
    "

S = "${UNPACKDIR}"

inherit systemd

SYSTEMD_SERVICE:${PN} = "trx40d8-config-migration.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

RDEPENDS:${PN} = "entity-manager"

do_install() {
    install -d ${D}${libexecdir}
    install -m 0755 ${UNPACKDIR}/trx40d8-config-migration \
        ${D}${libexecdir}/trx40d8-config-migration

    install -d ${D}${datadir}/trx40d8-config-migration
    install -m 0644 ${UNPACKDIR}/config-schema-version \
        ${D}${datadir}/trx40d8-config-migration/config-schema-version

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/trx40d8-config-migration.service \
        ${D}${systemd_system_unitdir}/trx40d8-config-migration.service
}
