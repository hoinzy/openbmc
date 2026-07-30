SUMMARY = "AMI Redfish USB host interface"
DESCRIPTION = "Create the source-isolated RNDIS interface expected by the TRX40 AMI firmware"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

inherit systemd

SRC_URI = " \
    file://ami-host-interface.sh \
    file://ami-host-network-config.sh \
    file://ami-host-interface.service \
    file://10-ami-usb0.network \
    "

SYSTEMD_SERVICE:${PN} = "ami-host-interface.service"

do_install() {
    install -d ${D}${libexecdir}
    install -m 0755 ${UNPACKDIR}/ami-host-interface.sh \
        ${D}${libexecdir}/ami-host-interface
    install -m 0755 ${UNPACKDIR}/ami-host-network-config.sh \
        ${D}${libexecdir}/ami-host-network-config

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/ami-host-interface.service \
        ${D}${systemd_system_unitdir}/

    # Keep an authoritative fallback in /usr/lib and seed the higher-priority
    # /etc copy. The service refreshes the latter over stale writable state on
    # upgrades.
    install -d ${D}${systemd_unitdir}/network
    install -m 0644 ${UNPACKDIR}/10-ami-usb0.network \
        ${D}${systemd_unitdir}/network/10-ami-usb0.network

    install -d ${D}${sysconfdir}/systemd/network
    install -m 0644 ${UNPACKDIR}/10-ami-usb0.network \
        ${D}${sysconfdir}/systemd/network/00-bmc-usb0.network
}

FILES:${PN} += " \
    ${libexecdir}/ami-host-network-config \
    ${systemd_unitdir}/network/10-ami-usb0.network \
    ${sysconfdir}/systemd/network/00-bmc-usb0.network \
    "
