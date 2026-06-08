#!/bin/sh
# Guest-side commands for the libusb virtual-device test, run inside a virtme-ng
# VM (see .github/workflows/libusb-vhid-test.yml). Kept as a file so a complex
# command line doesn't have to survive vng's argument parser.
#
# Runs as root in the guest, with the host filesystem mounted, cwd at the
# workspace root (which contains the host-built 'build' tree).
set -x

# The guest's modules.dep may be trimmed; regenerate it so dummy_hcd /
# raw_gadget (and their dependencies) resolve from the overlaid /lib/modules.
depmod -a || true
modprobe dummy_hcd || true
modprobe raw_gadget || true
ls -l /dev/raw-gadget || true

ctest --test-dir build --output-on-failure
rc=$?

echo "=== diag ==="
lsmod | grep -E "raw_gadget|dummy_hcd|udc" || true
ls -l /sys/bus/usb/devices/ 2>/dev/null || true
dmesg | tail -40 || true

echo "VNG_CTEST_EXIT=${rc}"
