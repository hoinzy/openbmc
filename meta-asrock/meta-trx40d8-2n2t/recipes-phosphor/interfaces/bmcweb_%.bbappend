FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append:trx40d8-2n2t = " \
    file://0001-bmcweb-add-AMI-host-inventory-routes.patch \
    file://ami_host_inventory.hpp \
    "

do_configure:prepend:trx40d8-2n2t() {
    install -m 0644 ${UNPACKDIR}/ami_host_inventory.hpp \
        ${S}/redfish-core/lib/ami_host_inventory.hpp
}
