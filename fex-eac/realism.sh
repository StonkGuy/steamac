#!/bin/sh
# OPTIONAL, runs as root inside the SteamOS guest. Makes the guest look less like a microVM to software that checks,
# following the hardware-realism advice in VRChat's "Using VRChat in a Virtual Machine" guide: plausible SMBIOS/DMI
# strings, a systemd-looking PID 1 and a desktop-style hostname. It does not touch the anti-cheat or its results.
# Override values through environment variables (REALISM_*). Optional for VRChat; may be useful for other games whose
# anti-cheat inspects the hardware identity.
#
#   sudo sh realism.sh          apply
#   sudo sh realism.sh undo     remove the mounts and restore the hostname
#
# Adapted from the research repo's scripts/vm/realism.sh. Two parts of the original do not apply to this guest, which
# is an aarch64 microVM booted from device tree (/sys/firmware/fdt) with no SMBIOS: this kernel exposes no DMI at all
# (no /sys/class/dmi, no /sys/devices/virtual/dmi) and no PCI bus (no /proc/bus/pci), so the DMI strings are staged
# into a fresh overlay on /sys/class and /sys/devices/virtual, and the PCI device list is skipped unless the guest
# actually has /proc/bus/pci. Idempotent; safe to re-run.
#
# Apache-2.0, like the rest of fex-eac/.
set -u
R=${REALISM_DIR:-/tmp/vrchat-fex-eac-realism}
S=$R/stage

# Detach anything we mounted before, so re-running does not stack mounts.
detach() {
  for t in /proc/1/cmdline /proc/1/comm /proc/bus/pci /sys/class /sys/devices/virtual; do
    mountpoint -q "$t" 2>/dev/null && umount -l "$t" 2>/dev/null
  done
}

case "${1:-}" in
undo)
  detach
  [ -f "$R/hostname.orig" ] && hostname "$(cat "$R/hostname.orig")" 2>/dev/null
  [ -f "$R/hostname.bak" ]  && cp "$R/hostname.bak" /etc/hostname 2>/dev/null
  rm -rf "$R"
  echo "realism undone (mounts detached, hostname restored)"
  exit 0;;
esac

detach
mkdir -p "$R"
# Save the real values once, before we overwrite anything, so a re-run cannot record the faked value as the original.
[ -f "$R/hostname.orig" ] || hostname > "$R/hostname.orig" 2>/dev/null
[ -f "$R/hostname.bak" ]  || cp -n /etc/hostname "$R/hostname.bak" 2>/dev/null
# Restage the contents; the saved originals above are deliberately left in place.
rm -rf "$S"; mkdir -p "$S/virt/dmi/id" "$S/class/dmi/id"

put() { for d in "$S/virt/dmi/id" "$S/class/dmi/id"; do printf '%s\n' "$2" > "$d/$1"; done; }
put sys_vendor      "${REALISM_VENDOR:-Gigabyte Technology Co., Ltd.}"
put product_name    "${REALISM_PRODUCT:-Z490 AORUS ELITE}"
put product_version "${REALISM_PRODUCT_VERSION:--CF}"
put board_vendor    "${REALISM_VENDOR:-Gigabyte Technology Co., Ltd.}"
put board_name      "${REALISM_PRODUCT:-Z490 AORUS ELITE}"
put board_version   "${REALISM_PRODUCT_VERSION:--CF}"
put bios_vendor     "${REALISM_BIOS_VENDOR:-American Megatrends International, LLC.}"
put bios_version    "${REALISM_BIOS_VERSION:-F10}"
put bios_date       "${REALISM_BIOS_DATE:-07/01/2021}"
put bios_release    "5.17"
put chassis_type    "3"
for f in board_serial board_asset_tag product_serial product_family product_sku chassis_vendor chassis_version chassis_serial chassis_asset_tag; do put $f "Default string"; done
# lowerdir-only overlays: read-only, so the fake DMI is visible without touching the kernel's sysfs.
mount -t overlay overlay -o "lowerdir=$S/virt:/sys/devices/virtual" /sys/devices/virtual 2>/dev/null
mount -t overlay overlay -o "lowerdir=$S/class:/sys/class" /sys/class 2>/dev/null

# PCI device list (host bridge, ISA bridge, a GPU-looking function). Skipped here: this guest has no PCI bus, and a
# tmpfs over /proc/bus would also hide the /proc/bus/input this guest does have.
if [ -d /proc/bus/pci ]; then
  mount -t tmpfs tmpfs /proc/bus/pci 2>/dev/null
  printf '0000\t80869b33\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t\n0008\t80869b44\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t\n0010\t10de1e87\t10\td0000000\tc0000000\t0\t0\t0\t0\t0\t1000000\t10000000\t0\t0\t0\t0\t0\t\n' > /proc/bus/pci/devices
fi

# PID 1 looks like systemd. This guest already runs systemd as PID 1, so the bind is cosmetic; keep it so a guest
# booting with a different init still presents systemd.
printf 'systemd\n' > "$S/comm"; printf '/usr/lib/systemd/systemd\0' > "$S/cmdline"
mount --bind "$S/comm" /proc/1/comm 2>/dev/null
mount --bind "$S/cmdline" /proc/1/cmdline 2>/dev/null

HN=${REALISM_HOSTNAME:-GAMING-PC}
hostname "$HN" 2>/dev/null
echo "$HN" > /etc/hostname 2>/dev/null
exit 0
