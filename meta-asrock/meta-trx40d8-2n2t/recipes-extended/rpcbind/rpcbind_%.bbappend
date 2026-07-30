# Network virtual media is an NFS client and mounts read-only with nolock.
# Keep the client utilities available without exposing a local RPC server.
SYSTEMD_AUTO_ENABLE:${PN}:trx40d8-2n2t = "disable"
