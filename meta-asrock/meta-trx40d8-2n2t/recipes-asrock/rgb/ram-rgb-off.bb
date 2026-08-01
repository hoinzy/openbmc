SUMMARY = "Disable ENE-controlled DRAM RGB on the TRX40D8-2N2T"
DESCRIPTION = "Programs both DRAM lighting banks through the BMC-owned I2C mux"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://ram-rgb-off \
    file://ram-rgb-off.service \
    "

S = "${UNPACKDIR}"

inherit systemd

SYSTEMD_SERVICE:${PN} = "ram-rgb-off.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

RDEPENDS:${PN} = "i2c-tools libgpiod-tools"

do_install() {
    install -d ${D}${sbindir}
    install -m 0755 ${UNPACKDIR}/ram-rgb-off \
        ${D}${sbindir}/trx40d8-ram-rgb-off

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/ram-rgb-off.service \
        ${D}${systemd_system_unitdir}/ram-rgb-off.service
}
