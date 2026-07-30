SUMMARY = "AMI Redfish USB host interface"
DESCRIPTION = "Create the source-isolated RNDIS interface expected by the TRX40 AMI firmware"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

inherit systemd

SRC_URI = " \
    file://ami-host-interface.sh \
    file://ami-host-interface.service \
    file://10-ami-usb0.network \
    "

SYSTEMD_SERVICE:${PN} = "ami-host-interface.service"

do_install() {
    install -d ${D}${libexecdir}
    install -m 0755 ${UNPACKDIR}/ami-host-interface.sh \
        ${D}${libexecdir}/ami-host-interface

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/ami-host-interface.service \
        ${D}${systemd_system_unitdir}/

    # Keep this in /etc so it takes precedence over generic network files and
    # remains the authoritative configuration for the source-isolated link.
    install -d ${D}${sysconfdir}/systemd/network
    install -m 0644 ${UNPACKDIR}/10-ami-usb0.network \
        ${D}${sysconfdir}/systemd/network/00-bmc-usb0.network
}

FILES:${PN} += "${sysconfdir}/systemd/network/00-bmc-usb0.network"
