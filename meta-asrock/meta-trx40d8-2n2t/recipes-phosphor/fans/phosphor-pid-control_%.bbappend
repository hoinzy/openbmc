FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append:trx40d8-2n2t = " \
    file://0001-dbus-keep-missing-acceptable-for-fan-pwm-pairs.patch \
    file://trx40d8-fan-full-speed \
    "

PACKAGECONFIG:append:trx40d8-2n2t = " \
    handle-missing-object-paths \
    offline-failsafe \
    "

inherit obmc-phosphor-systemd

SYSTEMD_SERVICE:${PN}:trx40d8-2n2t = "phosphor-pid-control.service"

do_install:append:trx40d8-2n2t() {
    install -d ${D}${bindir}
    install -m 0755 ${UNPACKDIR}/trx40d8-fan-full-speed \
        ${D}${bindir}/trx40d8-fan-full-speed
}

FILES:${PN}:append:trx40d8-2n2t = " \
    ${bindir}/trx40d8-fan-full-speed \
    "
