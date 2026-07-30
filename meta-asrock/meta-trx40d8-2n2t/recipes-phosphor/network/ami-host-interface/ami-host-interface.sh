#!/bin/sh

set -eu

gadget_root=/sys/kernel/config/usb_gadget
gadget_name=ami_host_interface
gadget_path="${gadget_root}/${gadget_name}"
udc=1e6a0000.usb-vhub:p2

stop_gadget()
{
    if [ ! -d "${gadget_path}" ]; then
        return
    fi
    if [ -e "${gadget_path}/UDC" ]; then
        printf '%s' "" > "${gadget_path}/UDC"
    fi
    rm -f "${gadget_path}/os_desc/c.1"
    rm -f "${gadget_path}/configs/c.1/rndis.usb0"
    rmdir "${gadget_path}/functions/rndis.usb0" 2>/dev/null || true
    rmdir "${gadget_path}/configs/c.1/strings/0x409" 2>/dev/null || true
    rmdir "${gadget_path}/configs/c.1" 2>/dev/null || true
    rmdir "${gadget_path}/strings/0x409" 2>/dev/null || true
    rmdir "${gadget_path}" 2>/dev/null || true
}

if [ "${1:-start}" = "stop" ]; then
    stop_gadget
    exit 0
fi

if [ ! -d "${gadget_root}" ]; then
    echo "USB gadget ConfigFS is unavailable" >&2
    exit 1
fi
if [ ! -e "/sys/class/udc/${udc}" ]; then
    echo "Dedicated AMI host-interface UDC ${udc} is unavailable" >&2
    exit 1
fi

stop_gadget
if grep -Fqx "${udc}" "${gadget_root}"/*/UDC 2>/dev/null; then
    echo "Dedicated AMI host-interface UDC ${udc} is already in use" >&2
    exit 1
fi

mkdir -p "${gadget_path}"

# The AMI UEFI RNDIS driver creates EFI_SIMPLE_NETWORK_PROTOCOL only for the
# identity exposed by the original BMC's eth.ko.  Keep these descriptors
# compatible so RedfishHi observes MediaPresent and starts before the OS boots.
printf '%s' 0x046b > "${gadget_path}/idVendor"
printf '%s' 0xffb0 > "${gadget_path}/idProduct"
printf '%s' 0x0100 > "${gadget_path}/bcdDevice"
printf '%s' 0x0200 > "${gadget_path}/bcdUSB"
printf '%s' 0x02 > "${gadget_path}/bDeviceClass"
printf '%s' 0x00 > "${gadget_path}/bDeviceSubClass"
printf '%s' 0x00 > "${gadget_path}/bDeviceProtocol"

mkdir -p "${gadget_path}/strings/0x409"
printf '%s' "American Megatrends Inc." \
    > "${gadget_path}/strings/0x409/manufacturer"
printf '%s' "Virtual Ethernet" \
    > "${gadget_path}/strings/0x409/product"
printf '%s' 1234567890 > "${gadget_path}/strings/0x409/serialnumber"

mkdir -p "${gadget_path}/configs/c.1/strings/0x409"
printf '%s' RNDIS \
    > "${gadget_path}/configs/c.1/strings/0x409/configuration"
printf '%s' 250 > "${gadget_path}/configs/c.1/MaxPower"

mkdir -p "${gadget_path}/functions/rndis.usb0"
printf '%s' 02:1a:11:00:00:17 \
    > "${gadget_path}/functions/rndis.usb0/dev_addr"
printf '%s' 02:1a:11:00:00:18 \
    > "${gadget_path}/functions/rndis.usb0/host_addr"
printf '%s' RNDIS \
    > "${gadget_path}/functions/rndis.usb0/os_desc/interface.rndis/compatible_id"
printf '%s' 5162001 \
    > "${gadget_path}/functions/rndis.usb0/os_desc/interface.rndis/sub_compatible_id"
(
    cd "${gadget_path}"
    ln -s functions/rndis.usb0 configs/c.1/rndis.usb0
)

# Microsoft OS descriptors let Windows and the AMI UEFI RNDIS driver identify
# the function without exposing another network on the BMC's LAN interfaces.
printf '%s' 1 > "${gadget_path}/os_desc/use"
printf '%s' 0xcd > "${gadget_path}/os_desc/b_vendor_code"
printf '%s' MSFT100 > "${gadget_path}/os_desc/qw_sign"
(
    cd "${gadget_path}"
    ln -s configs/c.1 os_desc/c.1
)

printf '%s' "${udc}" > "${gadget_path}/UDC"
