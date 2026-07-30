#!/bin/sh

set -eu

source=/usr/lib/systemd/network/10-ami-usb0.network
target=/etc/systemd/network/00-bmc-usb0.network

# The writable overlay can retain an older vendor network file across an
# image update.  Refresh the authoritative compatibility file before the
# gadget service starts so systemd-networkd cannot select the stale DHCP
# configuration.
if [ -r "$source" ]; then
    mkdir -p "$(dirname "$target")"
    cp "$source" "$target"
    chmod 0644 "$target"
fi

# Reconfigure immediately when this runs on an already-started system.  These
# calls are allowed to fail during early boot; networkd will apply the file
# when it starts or reloads later in the boot sequence.
if command -v networkctl >/dev/null 2>&1; then
    networkctl reload || true
    networkctl reconfigure usb0 || true
fi
