# Network virtual media is an NFS client and mounts read-only with nolock.
# Keep the client utilities available without exposing a local RPC server.
SYSTEMD_AUTO_ENABLE:${PN}:trx40d8-2n2t = "disable"

# Existing systems retain their writable /etc/passwd across an update and do
# not have the rpc account added by a newly introduced package.  The daemon is
# disabled above, so its runtime directory is neither needed nor usable.
do_install:append:trx40d8-2n2t() {
    rm -f ${D}${sysconfdir}/tmpfiles.d/rpcbind.conf
}
