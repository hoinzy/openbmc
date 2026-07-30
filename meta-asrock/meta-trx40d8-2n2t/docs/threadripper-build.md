# Building the TRX40D8-2N2T image on Threadripper

The build host is `threadripper` (`192.168.178.179`). The checkout and build
directory are:

```text
/home/ubuntu/openbmc-trx40-build/src-agent-f4b13f40b1
/home/ubuntu/openbmc-trx40-build/build-trx40d8-f4b13f40b1
```

The checkout is the `agent/trx40d8-initial-port` branch. Fetch the latest
branch before building:

```bash
cd /home/ubuntu/openbmc-trx40-build/src-agent-f4b13f40b1
git fetch origin agent/trx40d8-initial-port
git merge --ff-only FETCH_HEAD
```

## Start or resume a build

Run this as `ubuntu`. It is safe to run after an interrupted build; BitBake
will reuse completed work from the build cache.

```bash
cd /home/ubuntu/openbmc-trx40-build/src-agent-f4b13f40b1
source ./setup trx40d8-2n2t /home/ubuntu/openbmc-trx40-build/build-trx40d8-f4b13f40b1
export BB_NUMBER_THREADS=64
export PARALLEL_MAKE="-j 64"
bitbake obmc-phosphor-image
```

For a detached build with a persistent log:

```bash
buildroot=/home/ubuntu/openbmc-trx40-build
src="$buildroot/src-agent-f4b13f40b1"
log="$buildroot/build-$(git -C "$src" rev-parse --short HEAD).log"

nohup bash -c '
set -eo pipefail
original_userns=$(sysctl -n kernel.apparmor_restrict_unprivileged_userns)
restore() {
    sudo sysctl -q kernel.apparmor_restrict_unprivileged_userns="$original_userns" || true
}
trap restore EXIT HUP INT TERM
sudo sysctl -q kernel.apparmor_restrict_unprivileged_userns=0
cd /home/ubuntu/openbmc-trx40-build/src-agent-f4b13f40b1
source ./setup trx40d8-2n2t /home/ubuntu/openbmc-trx40-build/build-trx40d8-f4b13f40b1
export BB_NUMBER_THREADS=64
export PARALLEL_MAKE="-j 64"
bitbake obmc-phosphor-image
' >"$log" 2>&1 < /dev/null &
echo "Build log: $log"
```

The temporary AppArmor sysctl is needed on this host for the build tools. The
command restores its original value automatically when BitBake exits.

## Monitor the build

```bash
pgrep -af 'bitbake|bitbake-server'
tail -f /home/ubuntu/openbmc-trx40-build/build-<commit>.log
```

A successful build ends with a task summary showing all tasks succeeded. If a
network fetch fails, retry the same command first. For a crate fetch that has
repeatedly failed, download it into the build download cache and resume:

```bash
cd /home/ubuntu/openbmc-trx40-build/build-trx40d8-f4b13f40b1/downloads
curl -fL --retry 5 --retry-all-errors \
  -o bstr-1.12.1.crate \
  https://static.crates.io/crates/bstr/1.12.1/download
```

## Locate and verify the upload image

After a successful build, the web-update archive is under:

```text
/home/ubuntu/openbmc-trx40-build/build-trx40d8-f4b13f40b1/tmp/deploy/images/trx40d8-2n2t/
```

Select the file ending in `.static.mtd.tar`. Verify its manifest before
copying it off the host:

```bash
artifact=$(find /home/ubuntu/openbmc-trx40-build/build-trx40d8-f4b13f40b1/tmp/deploy/images/trx40d8-2n2t \
  -maxdepth 1 -type f -name '*.static.mtd.tar' -printf '%T@ %p\\n' | sort -nr | head -1 | cut -d' ' -f2-)
sha256sum "$artifact"
tar -xOf "$artifact" MANIFEST
```

The archive must contain `image-u-boot`, `kernel`, `rofs`, `rwfs`,
`MANIFEST`, and the signature files. Copy it to the workstation with legacy
SCP mode if required:

```bash
scp -O ubuntu@threadripper:"$artifact" .
```

Do not reboot the Threadripper while BitBake is running. A reboot interrupts
the build, but does not invalidate the existing sstate or download cache;
rerun the start/resume command afterward.
