#!/bin/bash
#
# Configure the host so a USB-tethered tablet is a streaming link, not an
# internet gateway. Run once; it survives reboots and re-plugs.
#
# See 90-stream-tablet-usb.conf for why this is needed.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONF_NAME="90-stream-tablet-usb.conf"
SRC="$SCRIPT_DIR/$CONF_NAME"
DEST="/etc/NetworkManager/conf.d/$CONF_NAME"

if [ ! -f "$SRC" ]; then
    echo "error: $SRC not found" >&2
    exit 1
fi

if ! command -v nmcli >/dev/null 2>&1; then
    echo "error: NetworkManager (nmcli) not found." >&2
    echo "If you use a different network stack, replicate this manually:" >&2
    echo "  give USB-tethering interfaces a high default-route metric." >&2
    exit 1
fi

echo "Installing $DEST ..."
sudo install -m 0644 "$SRC" "$DEST"

echo "Reloading NetworkManager configuration ..."
sudo nmcli general reload conf

# A generic wired profile - no interface-name, no MAC - attaches to whichever
# wired device NetworkManager happens to bring up. A USB tether presents as an
# ethernet device, so such a profile roams between the real NIC and the tablet,
# and activating one silently disconnects the other. Pin each active wired
# profile to the device it is currently on so the two stop fighting.
echo
echo "Checking for unpinned wired profiles ..."
while IFS=: read -r name device; do
    [ -z "$device" ] && continue
    iface=$(nmcli -g connection.interface-name connection show "$name" 2>/dev/null)
    mac=$(nmcli -g 802-3-ethernet.mac-address connection show "$name" 2>/dev/null)
    if [ -z "$iface" ] && [ -z "$mac" ]; then
        echo "  pinning '$name' to $device"
        sudo nmcli connection modify "$name" connection.interface-name "$device"
    fi
done < <(nmcli -t -f NAME,DEVICE,TYPE connection show --active 2>/dev/null \
         | awk -F: '$3=="802-3-ethernet" {print $1":"$2}')

echo
echo "Done. USB-tethered devices now get route metric 4000, so ethernet or"
echo "Wi-Fi will win the default route whenever either is available."
echo
echo "Current default route(s):"
ip route show default || true

# The metric only applies when the connection is next activated, so tell the
# user if the USB link is currently still holding the default route.
if ip route show default | grep -qE 'dev (enp[0-9a-z]*u[0-9]|usb[0-9]|rndis[0-9])'; then
    echo
    echo "NOTE: the USB link is still the active default route. It will move to"
    echo "      ethernet/Wi-Fi once one of those is up, or immediately if you"
    echo "      re-plug the tablet. Check that you have another route first:"
    echo "        nmcli device status"
fi
