# The vendor BIOS does not implement the OpenBMC SMBIOS blob hand-off.  Build
# the standard receiver so the host-side uploader can publish its table over
# the already-wired KCS interface.
PACKAGECONFIG:append = " smbios-ipmi-blob"
