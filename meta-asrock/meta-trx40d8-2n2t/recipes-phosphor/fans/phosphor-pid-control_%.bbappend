FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append:trx40d8-2n2t = " \
    file://0001-dbus-keep-missing-acceptable-for-fan-pwm-pairs.patch \
    file://0002-stepwise-interpolate-control-points.patch \
    file://trx40d8-fan-safe-speed \
    file://trx40d8-fan-safe-start.service \
    "

PACKAGECONFIG:append:trx40d8-2n2t = " \
    handle-missing-object-paths \
    offline-failsafe \
    "

inherit obmc-phosphor-systemd

SYSTEMD_SERVICE:${PN}:trx40d8-2n2t = " \
    phosphor-pid-control.service \
    trx40d8-fan-safe-start.service \
    "

do_install:append:trx40d8-2n2t() {
    install -d ${D}${bindir} ${D}${systemd_system_unitdir}
    install -m 0755 ${UNPACKDIR}/trx40d8-fan-safe-speed \
        ${D}${bindir}/trx40d8-fan-safe-speed
    install -m 0644 ${UNPACKDIR}/trx40d8-fan-safe-start.service \
        ${D}${systemd_system_unitdir}/trx40d8-fan-safe-start.service
}

FILES:${PN}:append:trx40d8-2n2t = " \
    ${bindir}/trx40d8-fan-safe-speed \
    ${systemd_system_unitdir}/trx40d8-fan-safe-start.service \
    "
