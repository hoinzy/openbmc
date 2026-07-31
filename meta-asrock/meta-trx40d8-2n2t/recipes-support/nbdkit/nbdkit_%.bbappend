# The base nbdkit recipe places every bundled plugin and filter in the main
# package.  Virtual Media only invokes the file and curl plugins; keeping the
# other examples and test-oriented modules pulls several unused libraries into
# this flash-constrained image.
PACKAGES =+ " \
    ${PN}-plugin-file \
    ${PN}-plugin-curl \
    ${PN}-plugins-extra \
    ${PN}-filters-extra \
    "

FILES:${PN}-plugin-file = " \
    ${libdir}/nbdkit/plugins/nbdkit-file-plugin.so \
    "
FILES:${PN}-plugin-curl = " \
    ${libdir}/nbdkit/plugins/nbdkit-curl-plugin.so \
    "
FILES:${PN}-plugins-extra = " \
    ${libdir}/nbdkit/plugins/*.so \
    "
FILES:${PN}-filters-extra = " \
    ${libdir}/nbdkit/filters/*.so \
    "
