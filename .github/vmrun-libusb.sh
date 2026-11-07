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

timeout --kill-after=3s 130s ctest --test-dir build \
    --output-on-failure --output-junit "$PWD/build/libusb-tests.xml"
rc=$?
if [ "$rc" -eq 0 ]; then
    python3 - <<'PY'
import sys
import xml.etree.ElementTree as ET

cases = ET.parse("build/libusb-tests.xml").getroot().findall(".//testcase")
expected = {"DeviceIO_libusb", "ReadInterrupt_libusb"}
passed = {case.attrib["name"] for case in cases
          if not any(case.find(tag) is not None for tag in ("skipped", "failure", "error"))}
if not expected <= passed:
    sys.exit("Both libusb virtual-device suites must run and pass without skips.")
PY
    rc=$?
fi

echo "=== diag ==="
lsmod | grep -E "raw_gadget|dummy_hcd|udc" || true
ls -l /sys/bus/usb/devices/ 2>/dev/null || true
dmesg | tail -40 || true

echo "VNG_CTEST_EXIT=${rc}"
