SUMMARY = "OpenBMC Virtual Media service"
DESCRIPTION = "Exposes browser and network-backed images to the host through USB mass storage"
HOMEPAGE = "https://github.com/Intel-BMC/virtual-media"

LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=e3fc50a88d0a364313df4b21ef20c29e"

SRC_URI = " \
    git://github.com/Intel-BMC/virtual-media.git;branch=main;protocol=https \
    file://0001-virtual-media-add-NFS-image-support.patch \
    file://0002-virtual-media-list-images-on-SMB-and-NFS-shares.patch \
    file://0003-virtual-media-use-Boost.Process-v1-compatibility-API.patch \
    file://0004-virtual-media-update-for-current-sdbusplus-and-Boost.patch \
    file://virtual-media.json \
    "
SRCREV = "1306c2132bdb9d5ad6deb00cd8a1d920753f7ea7"

PV = "0.1+git${SRCPV}"

DEPENDS = " \
    boost \
    nlohmann-json \
    sdbusplus \
    systemd \
    udev \
    "

RDEPENDS:${PN} = " \
    nbd-client \
    nbdkit \
    nbdkit-plugin-curl \
    nbdkit-plugin-file \
    nfs-utils-mount \
    "

inherit meson pkgconfig systemd

bindir = "${sbindir}"

EXTRA_OEMESON = " \
    -Dlegacy-mode=enabled \
    -Dtests=disabled \
    "

SYSTEMD_SERVICE:${PN} = "xyz.openbmc_project.VirtualMedia.service"

do_install:append() {
    install -m 0644 ${UNPACKDIR}/virtual-media.json \
        ${D}${sysconfdir}/virtual-media.json
}
