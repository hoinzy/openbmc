SUMMARY = "AMI host firmware inventory receiver"
DESCRIPTION = "Receive ASRock AMI firmware inventory over host KCS and publish OpenBMC inventory objects"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

DEPENDS = "phosphor-ipmi-host phosphor-logging sdbusplus"

inherit meson pkgconfig obmc-phosphor-ipmiprovider-symlink

SRC_URI = " \
    file://meson.build \
    file://ami_inventory.hpp \
    file://ami_inventory.cpp \
    file://ami_inventory_handler.cpp \
    file://test_ami_inventory.cpp \
    "

S = "${UNPACKDIR}"

HOSTIPMI_PROVIDER_LIBRARY += "libamiinventory.so"

FILES:${PN}:append = " ${libdir}/ipmid-providers/lib*${SOLIBS}"
FILES:${PN}:append = " ${libdir}/host-ipmid/lib*${SOLIBS}"
FILES:${PN}-dev:append = " ${libdir}/ipmid-providers/lib*${SOLIBSDEV}"
