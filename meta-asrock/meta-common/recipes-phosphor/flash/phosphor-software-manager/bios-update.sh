#!/bin/bash

die() { logger -s -t bios-update "Error: $*"; exit 1; }
info() { logger -s -t bios-update "$*"; }

# shellcheck disable=SC1091
bios_update_config="${BIOS_UPDATE_CONFIG:-/etc/default/bios-update}"
. "$bios_update_config" || die "Failed: unable to load $bios_update_config"

[ -n "$BIOS_UPDATE_MAGIC_OFFSET" ] || die "BIOS_UPDATE_MAGIC_OFFSET not set"
[ -n "$BIOS_UPDATE_MAGIC" ] || die "BIOS_UPDATE_MAGIC not set"
[ -n "$BIOS_UPDATE_SIZE" ] || die "BIOS_UPDATE_SIZE not set"
[ "$((BIOS_UPDATE_SIZE % (1 << 20)))" = 0 ] || die "BIOS_UPDATE_SIZE must be a whole number of MiB"

declare -A prep_gpios_pids
declare -a prepared_gpios

bios_flash_spidev="${BIOS_UPDATE_SPI_DEVICE:-1e630000.spi}"
smc_drvdir="${BIOS_UPDATE_SMC_DRIVER_DIR:-/sys/bus/platform/drivers/spi-aspeed-smc}"
bios_flash_devdir="${BIOS_UPDATE_SPI_DEVICE_DIR:-/sys/bus/platform/devices/$bios_flash_spidev}"
bios_flash_attached=false
bios_flash_override_set=false

hoststate_svc="xyz.openbmc_project.State.Host"
hoststate_path="/xyz/openbmc_project/state/host0"
hoststate_intf="xyz.openbmc_project.State.Host"
hoststate_prop="CurrentHostState"
hoststate_off="xyz.openbmc_project.State.Host.HostState.Off"

check_host_off()
{
	local state
	state="$(busctl get-property "$hoststate_svc" "$hoststate_path" \
			"$hoststate_intf" "$hoststate_prop")"
	if [ "$state" != "s \"$hoststate_off\"" ]; then
		die "host must be off before performing BIOS update"
	fi
}

# sets variables (gpioset background PIDs and bios flash mtd chardev,
# commented as "global") for later use
attach_bios_flash()
{
	if [ "${BIOS_UPDATE_MANUAL_BIND:-false}" = true ] && \
			[ -r "$bios_flash_devdir/driver_override" ]; then
		local current_override
		current_override="$(cat "$bios_flash_devdir/driver_override")"
		case "$current_override" in
		''|'(null)') ;;
		spi-aspeed-smc) bios_flash_override_set=true;;
		*) die "unexpected BIOS SPI driver override: $current_override";;
		esac
	fi

	if [ -L "$smc_drvdir/$bios_flash_spidev" ]; then
		info "BIOS flash is already attached"
		bios_flash_attached=true
	fi

	for gpio in "${BIOS_UPDATE_PREP_GPIOS[@]}" ; do
		read -ra kv <<<"${gpio/=/ }"
		info "Setting ${kv[0]} to ${kv[1]}..."
		gpio="$(gpiofind "${kv[0]}")" || die "Failed to find ${kv[0]} GPIO"
		# shellcheck disable=SC2086
		gpioset -m signal ${gpio}="${kv[1]}" &
		prep_gpios_pids[${kv[0]}]=$! # global
		prepared_gpios+=("${kv[0]}=${kv[1]}")
		sleep 1
	done

	if ! $bios_flash_attached; then
		if [ "${BIOS_UPDATE_MANUAL_BIND:-false}" = true ]; then
			[ -w "$bios_flash_devdir/driver_override" ] || \
				die "BIOS SPI driver override is unavailable"
			info "Enabling manual BIOS SPI driver binding..."
			echo spi-aspeed-smc > "$bios_flash_devdir/driver_override" || \
				die "failed to enable manual BIOS SPI driver binding"
			bios_flash_override_set=true
		fi

		info "Attaching BIOS flash..."
		echo "$bios_flash_spidev" > "$smc_drvdir/bind" || die "failed to attach aspeed-smc driver to BIOS SPI flash"
		bios_flash_attached=true
	fi

	if [ -n "${BIOS_UPDATE_MTD_DEVICE:-}" ]; then
		bios_mtd_dev="$BIOS_UPDATE_MTD_DEVICE" # global
	else
		local tmp
		tmp="$(grep -xl bios /sys/class/mtd/*/name)"
		tmp="${tmp%/name}"
		tmp="${tmp##*/}"
		bios_mtd_dev="/dev/$tmp" # global
	fi
	[ -c "$bios_mtd_dev" ] || die "bios mtd chardev not found"
}

restore_bios_flash()
{
	local status=0

	if $bios_flash_attached; then
		info "Detaching BIOS flash..."
		if ! echo "$bios_flash_spidev" > "$smc_drvdir/unbind"; then
			info "Error: failed to detach aspeed-smc driver from BIOS SPI flash"
			status=1
		else
			bios_flash_attached=false
		fi
	fi

	if $bios_flash_override_set && ! $bios_flash_attached; then
		info "Disabling manual BIOS SPI driver binding..."
		if ! echo > "$bios_flash_devdir/driver_override"; then
			info "Error: failed to clear manual BIOS SPI driver binding"
			status=1
		else
			bios_flash_override_set=false
		fi
	fi

	# Detach in reverse order
	for ((i = ${#prepared_gpios[@]} - 1; i >= 0; i--)) ; do
		read -ra kv <<<"${prepared_gpios[i]/=/ }"
		notvalue=$((! kv[1]))
		info "Resetting ${kv[0]} to ${notvalue}..."
		kill -INT "${prep_gpios_pids[${kv[0]}]}" 2>/dev/null || true
		wait "${prep_gpios_pids[${kv[0]}]}" 2>/dev/null || true
		if ! gpio="$(gpiofind "${kv[0]}")"; then
			info "Error: failed to find ${kv[0]} GPIO while restoring BIOS access"
			status=1
			continue
		fi
		# shellcheck disable=SC2086
		if ! gpioset -m exit ${gpio}="$notvalue"; then
			info "Error: failed to restore ${kv[0]} GPIO"
			status=1
		fi
		sleep 1
	done
	prepared_gpios=()

	return "$status"
}

cleanup_bios_flash()
{
	local status=$?

	trap - EXIT
	if ! restore_bios_flash; then
		status=1
	fi
	exit "$status"
}

trap cleanup_bios_flash EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

check_bios_image()
{
	if [ ! -r "$1" ]; then
		info "Error: can't read BIOS image $1"
		return 1
	fi

	local imgsize magic
	imgsize="$(stat -c %s "$1")"
	if [ "$imgsize" != "${BIOS_UPDATE_SIZE}" ]; then
		info "Error: invalid BIOS image (wrong size)"
		return 1
	fi

	magic="$(dd if="$1" bs=1 count=4 skip="${BIOS_UPDATE_MAGIC_OFFSET}" 2>/dev/null | hexdump -e '3/1 "%02x" "%02x\n"')"
	if [ "$magic" != "${BIOS_UPDATE_MAGIC}" ]; then
		info "Error: invalid BIOS image (magic number mismatch)"
		return 1
	fi
}

flash_bios_image()
{
	local bios_img="$1"

	info "Checking BIOS image..."
	check_bios_image "$bios_img" || die "BIOS image validation failed"

	info "Checking host state..."
	check_host_off

	attach_bios_flash

	info "Writing BIOS image to SPI flash..."
	if flashcp -v "$bios_img" "$bios_mtd_dev"; then
		info "Flash update successful"
		local status=0
	else
		info "Error updating flash! (proceeding with detach)"
		local status=1
	fi

	restore_bios_flash || status=1

	return "$status"
}

read_bios_image()
{
	local output="$1"

	[ ! -e "$output" ] || die "refusing to overwrite existing file $output"

	info "Checking host state..."
	check_host_off

	attach_bios_flash

	info "Reading BIOS image from SPI flash..."
	umask 077
	if ! dd if="$bios_mtd_dev" of="$output" bs=1M \
			count="$((BIOS_UPDATE_SIZE / (1 << 20)))"; then
		rm -f "$output"
		die "failed to read BIOS SPI flash"
	fi

	if ! restore_bios_flash; then
		rm -f "$output"
		die "failed to restore host access after reading BIOS SPI flash"
	fi

	info "Checking captured BIOS image..."
	if ! check_bios_image "$output"; then
		rm -f "$output"
		die "captured BIOS image failed validation"
	fi

	info "BIOS backup complete: $output"
}

# HACK: for unknown reasons, on e3c246d4i, the host seems to refuse to power on
# after we switch the BIOS SPI flash to the BMC and back to the host,
# but it recovers if we hold the POWER_OUT GPIO as in a press-and-hold
# of the front-panel power button (even though the host is already
# powered off).  I don't really know what's going on here.
do_power_hack()
{
	# power-control holds the POWER_OUT gpio, so we need to stop it if it's on
	local powerctl_svc="xyz.openbmc_project.Chassis.Control.Power.service"
	local powerhack_time=8
	local psout_gpio

	psout_gpio="$(gpiofind "${BIOS_UPDATE_POWER_GPIO}")"

	prev_powerctl_state="$(systemctl show --property=ActiveState "$powerctl_svc")"
	if [ "$prev_powerctl_state" = "ActiveState=active" ]; then
		systemctl stop "$powerctl_svc" || info "Warning: failed to stop $powerctl_svc"
	fi

	info "Holding host power line for $powerhack_time seconds..."

	# shellcheck disable=SC2086
	gpioset -m time -s "$powerhack_time" ${psout_gpio}=0 || die "Failed to assert ${BIOS_UPDATE_POWER_GPIO}) GPIO"
	# shellcheck disable=SC2086
	gpioset ${psout_gpio}=1 || die "Failed to release ${BIOS_UPDATE_POWER_GPIO} GPIO"

	info "Host power line released..."

	# if the power-control service was for some reason not running to
	# start with, leave it that way.
	if [ "$prev_powerctl_state" = "ActiveState=active" ]; then
		systemctl start "$powerctl_svc" || info "Warning: failed to restore $powerctl_svc"
	fi
}

# Find the image file within a /tmp/images/$IMGHASH directory (should
# be the one file not named MANIFEST).  We could be a little more
# automagic and run check_bios_image on each candidate in case there's
# more than one (discarding any that fail), but for now we'll keep it
# simple and not try to handle anything unexpected.
find_imgfile()
{
	[ -d "$1" ] || die "$1: not a directory"
	local img='' path name
	for path in "$1"/*; do
		name="$(basename "$path")"
		if [ "$name" = "MANIFEST" ]; then
			# ignore MANIFEST file
			continue
		elif [ -n "$img" ]; then
			# if we've already hit a non-MANIFEST file, bail
			die "multiple potential image files in $1"
		else
			img="$path"
		fi
	done
	[ -n "$img" ] || die "no image file found in $1"
	echo "$img"
}

# when invoked by the systemd unit as part of the web-UI machinery we
# get passed /tmp/images/$IMGHASH (directory containing the BIOS
# image), but for manual use it's nice to be able to just pass the raw
# image file directly, so we support both, differentiated by a '-d'
# flag.
imgdir_mode=false
read_mode=false
read_output=''

while getopts dr: opt; do
	case "$opt" in
	d) imgdir_mode=true;;
	r) read_mode=true; read_output="$OPTARG";;
	*) exit 1;;
	esac
done

shift $((OPTIND-1))

if $read_mode; then
	! $imgdir_mode || die "-d and -r cannot be used together"
	[ $# = 0 ] || die "usage: $0 -r OUTPUT_IMAGE"
	read_bios_image "$read_output"
	exit 0
fi

[ $# = 1 ] || die "usage: $0 [ BIOS_IMAGE | -d IMAGE_DIR | -r OUTPUT_IMAGE ]"

if $imgdir_mode; then
	imgfile="$(find_imgfile "$1")"
else
	imgfile="$1"
fi

if flash_bios_image "$imgfile"; then
	info "BIOS update complete."
else
	die "BIOS update failed!"
fi

if [ -n "$BIOS_UPDATE_POWER_GPIO" ]; then
	do_power_hack
fi

info "Done."
