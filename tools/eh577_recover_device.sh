#!/usr/bin/env bash
# Recover a transport-wedged EH577 (1c7a:0577) — i.e. the device is present on
# the bus but the driver times out on the very first pre-init bulk write
# ("Timeout sending first pre-init packet ...").
#
# This does a sysfs driver UNBIND/REBIND, which re-runs USB enumeration + probe
# for the device WITHOUT issuing a USB-level electrical reset. (g_usb_device_reset
# / a USB port reset is known to wedge this hardware until a cold reboot — do not
# use it here.) It also stops fprintd so nothing re-grabs the freed device.
#
# Run as root:
#   sudo ./tools/eh577_recover_device.sh
#
# If the device still times out after this, PHYSICALLY UNPLUG AND REPLUG the
# sensor — a real replug re-enumerates from scratch and is the reliable recovery.

set -uo pipefail

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  echo "Must run as root:  sudo $0"
  exit 1
fi

VID=1c7a
PID=0577

# Stop fprintd + its socket activation so it does not re-claim the device.
systemctl stop fprintd fprintd.socket 2>/dev/null || true
pkill -f fprintd 2>/dev/null || true

# Locate the sysfs node (e.g. "3-3") for the EH577.
NODE=""
for d in /sys/bus/usb/devices/*/; do
  [[ -r "${d}idVendor" && -r "${d}idProduct" ]] || continue
  if [[ "$(cat "${d}idVendor")" == "$VID" && "$(cat "${d}idProduct")" == "$PID" ]]; then
    NODE="$(basename "$d")"
    break
  fi
done

if [[ -z "$NODE" ]]; then
  echo "EH577 ($VID:$PID) not found on USB."
  echo "Physically replug the sensor, then re-run this script or ./tools/capture12.sh."
  exit 1
fi

SYS="/sys/bus/usb/devices/$NODE"
echo "EH577 sysfs node: $NODE"
echo "  before: control=$(cat "$SYS/power/control" 2>/dev/null) status=$(cat "$SYS/power/runtime_status" 2>/dev/null)"

# Keep runtime PM from suspending it.
echo on > "$SYS/power/control" 2>/dev/null || true

# Unbind then rebind from the generic 'usb' driver: re-runs enumeration/probe,
# no electrical reset.
echo "Unbinding $NODE ..."
echo "$NODE" > /sys/bus/usb/drivers/usb/unbind 2>/dev/null || true
sleep 1
echo "Rebinding $NODE ..."
echo "$NODE" > /sys/bus/usb/drivers/usb/bind 2>/dev/null || true
sleep 1

if lsusb | grep -q "$VID:$PID"; then
  echo "Device present after rebind."
  echo "Now try:  ./tools/capture12.sh"
  echo "If init STILL times out, physically unplug and replug the sensor."
else
  echo "Device did not re-enumerate after rebind — physically unplug and replug it."
  exit 1
fi
