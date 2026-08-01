#!/bin/bash

set -eu

script_dir="$(cd "$(dirname "$0")/.." && pwd)"
updater="$script_dir/bios-update.sh"
test_tmp="$(mktemp -d)"

cleanup()
{
	find "$test_tmp" -depth -delete
}
trap cleanup EXIT

mkdir -p "$test_tmp/bin" "$test_tmp/driver" "$test_tmp/device"
: > "$test_tmp/driver/bind"
: > "$test_tmp/driver/unbind"
: > "$test_tmp/device/driver_override"

cat > "$test_tmp/config" <<'EOF'
BIOS_UPDATE_PREP_GPIOS=(HOST_BIOS_BMC_SELECT_N=0)
BIOS_UPDATE_GPIO_RELEASE_USEC=100000
BIOS_UPDATE_RELEASE_INPUT_GPIOS=(HOST_BIOS_BMC_SELECT_N)
BIOS_UPDATE_MANUAL_BIND=true
BIOS_UPDATE_MAGIC_OFFSET=0
BIOS_UPDATE_MAGIC=00000000
BIOS_UPDATE_SIZE=$((1 << 20))
EOF

cat > "$test_tmp/bin/busctl" <<'EOF'
#!/bin/sh
echo 's "xyz.openbmc_project.State.Host.HostState.Off"'
EOF

cat > "$test_tmp/bin/dd" <<'EOF'
#!/bin/sh
case " $* " in
*" of="*)
	[ "${MOCK_DD_FAIL:-0}" = 0 ] || exit 1
	;;
esac
exec /bin/dd "$@"
EOF

cat > "$test_tmp/bin/logger" <<'EOF'
#!/bin/sh
exit 0
EOF

cat > "$test_tmp/bin/gpiofind" <<'EOF'
#!/bin/sh
[ "$1" = HOST_BIOS_BMC_SELECT_N ] || exit 1
echo "gpiochip0 73"
EOF

cat > "$test_tmp/bin/gpioset" <<'EOF'
#!/bin/sh
echo "$*" >> "$MOCK_GPIO_LOG"
exit 0
EOF

cat > "$test_tmp/bin/gpioget" <<'EOF'
#!/bin/sh
echo "$*" >> "$MOCK_GPIO_GET_LOG"
echo 1
EOF

cat > "$test_tmp/bin/sleep" <<'EOF'
#!/bin/sh
exit 0
EOF

cat > "$test_tmp/bin/stat" <<'EOF'
#!/bin/sh
if [ "$1" = -c ] && [ "$2" = %s ]; then
	wc -c < "$3" | tr -d ' '
	exit 0
fi
exec /usr/bin/stat "$@"
EOF

chmod +x "$test_tmp/bin/"*

run_updater()
{
	env \
		PATH="$test_tmp/bin:$PATH" \
		BIOS_UPDATE_CONFIG="$test_tmp/config" \
		BIOS_UPDATE_SMC_DRIVER_DIR="$test_tmp/driver" \
		BIOS_UPDATE_SPI_DEVICE_DIR="$test_tmp/device" \
		BIOS_UPDATE_MTD_DEVICE=/dev/zero \
		MOCK_GPIO_LOG="$test_tmp/gpioset.log" \
		MOCK_GPIO_GET_LOG="$test_tmp/gpioget.log" \
		bash "$updater" "$@"
}

backup="$test_tmp/backup.bin"
run_updater -r "$backup"

[ "$(wc -c < "$backup" | tr -d ' ')" = 1048576 ]
[ "$(cat "$test_tmp/driver/bind")" = 1e630000.spi ]
[ "$(cat "$test_tmp/driver/unbind")" = 1e630000.spi ]
[ -z "$(tr -d '\n' < "$test_tmp/device/driver_override")" ]
[ "$(sed -n '1p' "$test_tmp/gpioset.log")" = \
	"-m signal gpiochip0 73=0" ]
[ "$(sed -n '2p' "$test_tmp/gpioset.log")" = \
	"-m time -u 100000 gpiochip0 73=1" ]
[ "$(sed -n '1p' "$test_tmp/gpioget.log")" = "gpiochip0 73" ]

prebound_backup="$test_tmp/prebound.bin"
: > "$test_tmp/driver/bind"
: > "$test_tmp/driver/unbind"
echo spi-aspeed-smc > "$test_tmp/device/driver_override"
ln -s "$test_tmp/device" "$test_tmp/driver/1e630000.spi"
run_updater -r "$prebound_backup"
unlink "$test_tmp/driver/1e630000.spi"

[ ! -s "$test_tmp/driver/bind" ]
[ "$(cat "$test_tmp/driver/unbind")" = 1e630000.spi ]
[ -z "$(tr -d '\n' < "$test_tmp/device/driver_override")" ]

if run_updater -r "$backup"; then
	echo "updater overwrote an existing backup" >&2
	exit 1
fi

: > "$test_tmp/driver/unbind"
failed_backup="$test_tmp/failed.bin"
if MOCK_DD_FAIL=1 run_updater -r "$failed_backup"; then
	echo "updater accepted a failed flash read" >&2
	exit 1
fi

[ ! -e "$failed_backup" ]
[ "$(cat "$test_tmp/driver/unbind")" = 1e630000.spi ]
[ -z "$(tr -d '\n' < "$test_tmp/device/driver_override")" ]

cat > "$test_tmp/config" <<'EOF'
BIOS_UPDATE_PREP_GPIOS=(HOST_BIOS_BMC_SELECT_N=0)
BIOS_UPDATE_MANUAL_BIND=true
BIOS_UPDATE_MAGIC_OFFSET=0
BIOS_UPDATE_MAGIC=00000000
BIOS_UPDATE_SIZE=$((1 << 20))
EOF
: > "$test_tmp/gpioset.log"
: > "$test_tmp/gpioget.log"
legacy_backup="$test_tmp/legacy-backup.bin"
run_updater -r "$legacy_backup"

[ "$(sed -n '1p' "$test_tmp/gpioset.log")" = \
	"-m signal gpiochip0 73=0" ]
[ "$(sed -n '2p' "$test_tmp/gpioset.log")" = \
	"-m exit gpiochip0 73=1" ]
[ ! -s "$test_tmp/gpioget.log" ]

echo "bios-update backup tests passed"
