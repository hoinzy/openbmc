FILESEXTRAPATHS:prepend:trx40d8-2n2t := "${THISDIR}/jsnbd:"

SRC_URI:append:trx40d8-2n2t = " \
    file://0001-nbd-proxy-drop-incompatible-nonetlink-option.patch \
    "
