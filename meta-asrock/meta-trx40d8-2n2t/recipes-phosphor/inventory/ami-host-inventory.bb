SUMMARY = "AMI host firmware inventory receiver"
DESCRIPTION = "Stage AMI RfInventory JSON and publish standard OpenBMC host inventory objects"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

DEPENDS = "boost gtest nlohmann-json phosphor-dbus-interfaces phosphor-logging sdbusplus"

inherit meson pkgconfig systemd

SRC_URI = " \
    file://ami_inventory.hpp \
    file://ami_inventory.cpp \
    file://ami_bios.hpp \
    file://ami_bios.cpp \
    file://ami_inventory_daemon.cpp \
    file://test_ami_inventory.cpp \
    file://test_ami_bios.cpp \
    file://meson.build \
    file://meson.options \
    file://ami-host-inventory.service \
    "

S = "${UNPACKDIR}"

EXTRA_OEMESON = "-Dtests=enabled"

SYSTEMD_SERVICE:${PN} = "ami-host-inventory.service"

do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/ami-host-inventory.service \
        ${D}${systemd_system_unitdir}/
}
