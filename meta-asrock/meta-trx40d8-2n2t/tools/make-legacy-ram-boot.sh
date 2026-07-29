#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 FIT_IMAGE OUTPUT_BUNDLE" >&2
    exit 2
fi

fit_image=$1
output_bundle=$2
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

kernel_load=0x81000000
initramfs_load=0x84000000
bundle_load=0x83000000
page_size=4096

dumpimage -T flat_dt -p 0 -o "$work_dir/zImage" "$fit_image"
dumpimage -T flat_dt -p 1 -o "$work_dir/board.dtb" "$fit_image"
dumpimage -T flat_dt -p 2 -o "$work_dir/initramfs.cpio" "$fit_image"

kernel_size=$(wc -c <"$work_dir/zImage" | tr -d ' ')
initramfs_size=$(wc -c <"$work_dir/initramfs.cpio" | tr -d ' ')
initramfs_end=$((initramfs_load + initramfs_size))

fdtput -t s "$work_dir/board.dtb" /chosen bootargs \
    "console=ttyS4,115200 earlycon rdinit=/bin/sh"
fdtput -t x "$work_dir/board.dtb" /chosen linux,initrd-start \
    "$(printf '%x' "$initramfs_load")"
fdtput -t x "$work_dir/board.dtb" /chosen linux,initrd-end \
    "$(printf '%x' "$initramfs_end")"

mkimage -A arm -O linux -T kernel -C none \
    -a "$kernel_load" -e "$kernel_load" \
    -n "OpenBMC TRX40 RAM boot" \
    -d "$work_dir/zImage" "$work_dir/uImage"

uimage_size=$(wc -c <"$work_dir/uImage" | tr -d ' ')
initramfs_offset=$(((uimage_size + page_size - 1) / page_size * page_size))
dtb_offset=$(((initramfs_offset + initramfs_size + page_size - 1) / page_size * page_size))
dtb_size=$(wc -c <"$work_dir/board.dtb" | tr -d ' ')

cp "$work_dir/uImage" "$output_bundle"
truncate -s "$initramfs_offset" "$output_bundle"
dd if="$work_dir/initramfs.cpio" of="$output_bundle" bs=1 \
    seek="$initramfs_offset" conv=notrunc status=none
truncate -s "$dtb_offset" "$output_bundle"
dd if="$work_dir/board.dtb" of="$output_bundle" bs=1 \
    seek="$dtb_offset" conv=notrunc status=none

echo "bundle=$output_bundle"
echo "bundle_load=$(printf '0x%x' "$bundle_load")"
echo "kernel_size=$(printf '0x%x' "$kernel_size")"
echo "initramfs_source=$(printf '0x%x' "$((bundle_load + initramfs_offset))")"
echo "initramfs_size=$(printf '0x%x' "$initramfs_size")"
echo "initramfs_load=$(printf '0x%x' "$initramfs_load")"
echo "dtb_source=$(printf '0x%x' "$((bundle_load + dtb_offset))")"
echo "dtb_size=$(printf '0x%x' "$dtb_size")"
echo "dtb_load=0x82000000"
sha256sum "$output_bundle"
