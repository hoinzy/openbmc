SUMMARY = "ASRock Rack TRX40D8-2N2T TR1 temperature sensor"
DESCRIPTION = "Publishes the NCT6796D VIN3 thermistor as a native OpenBMC temperature sensor"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://trx40d8-tr1-sensor \
    file://trx40d8-tr1-sensor.service \
    "

S = "${UNPACKDIR}"

inherit systemd

SYSTEMD_SERVICE:${PN} = "trx40d8-tr1-sensor.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

RDEPENDS:${PN} = "dbus-sensors"

do_install() {
    install -d ${D}${sbindir}
    install -m 0755 ${UNPACKDIR}/trx40d8-tr1-sensor \
        ${D}${sbindir}/trx40d8-tr1-sensor

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/trx40d8-tr1-sensor.service \
        ${D}${systemd_system_unitdir}/trx40d8-tr1-sensor.service
}
