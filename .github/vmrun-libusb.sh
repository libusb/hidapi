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

rc=0
for test in DeviceIO_libusb HotplugAPI_libusb Hotplug_libusb; do
    listed=$(ctest --test-dir build -N -R "^${test}$" 2>&1)
    listed_rc=$?
    printf '%s\n' "$listed"
    if [ "$listed_rc" -ne 0 ] || ! printf '%s\n' "$listed" | grep -q 'Total Tests: 1'; then
        echo "Required CTest case '${test}' was not found."
        rc=1
        continue
    fi

    result=$(ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build -R "^${test}$" --output-on-failure 2>&1)
    result_rc=$?
    printf '%s\n' "$result"
    if [ "$result_rc" -ne 0 ] || ! printf '%s\n' "$result" | grep -Eq "^[[:space:]]*1/1 Test #[0-9]+: ${test} .* [P]assed[[:space:]]+[0-9]+([.][0-9]+)?[[:space:]]+sec[[:space:]]*$"; then
        echo "Required CTest case '${test}' did not pass."
        rc=1
    fi
done

echo "=== diag ==="
lsmod | grep -E "raw_gadget|dummy_hcd|udc" || true
ls -l /sys/bus/usb/devices/ 2>/dev/null || true
dmesg | tail -40 || true

echo "VNG_CTEST_EXIT=${rc}"
