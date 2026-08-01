SUMMARY = "TRX40D8 dual management-interface network preference"
DESCRIPTION = "Prefer dedicated Ethernet while keeping NCSI available as a fallback"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = " \
    file://10-dedicated-route-metric.conf \
    file://10-ncsi-route-metric.conf \
    file://60-trx40d8-arp.conf \
    "

S = "${UNPACKDIR}"

inherit allarch

do_install() {
    install -d \
        ${D}${systemd_unitdir}/network/00-bmc-eth0.network.d \
        ${D}${systemd_unitdir}/network/00-bmc-eth1.network.d \
        ${D}${libdir}/sysctl.d

    install -m 0644 ${UNPACKDIR}/10-dedicated-route-metric.conf \
        ${D}${systemd_unitdir}/network/00-bmc-eth0.network.d/10-route-metric.conf
    install -m 0644 ${UNPACKDIR}/10-ncsi-route-metric.conf \
        ${D}${systemd_unitdir}/network/00-bmc-eth1.network.d/10-route-metric.conf
    install -m 0644 ${UNPACKDIR}/60-trx40d8-arp.conf \
        ${D}${libdir}/sysctl.d/60-trx40d8-arp.conf
}

FILES:${PN} += " \
    ${systemd_unitdir}/network/00-bmc-eth0.network.d/10-route-metric.conf \
    ${systemd_unitdir}/network/00-bmc-eth1.network.d/10-route-metric.conf \
    ${libdir}/sysctl.d/60-trx40d8-arp.conf \
    "
