FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

DEPENDS:append:trx40d8-2n2t = " phosphor-dbus-interfaces"

SRC_URI:append:trx40d8-2n2t = " \
    file://0001-bmcweb-add-AMI-host-inventory-routes.patch \
    file://0002-bmcweb-expose-bios-settings-manager.patch \
    file://0003-bmcweb-allow-same-origin-bios-ui.patch \
    file://0004-virtual-media-accept-NFS-image-URLs.patch \
    file://0005-virtual-media-add-OEM-share-image-listing-action.patch \
    file://ami_host_inventory.hpp \
    "

# AMI RedfishHi uses TLS 1.2 and cannot negotiate bmcweb's default TLS 1.3-only
# profile. Keep TLS 1.3 while enabling the Mozilla intermediate TLS 1.2 cipher
# set on this firmware-facing board.
EXTRA_OEMESON:append:trx40d8-2n2t = " -Dtls-profile=intermediate"

do_configure:prepend:trx40d8-2n2t() {
    install -m 0644 ${UNPACKDIR}/ami_host_inventory.hpp \
        ${S}/redfish-core/lib/ami_host_inventory.hpp
}
