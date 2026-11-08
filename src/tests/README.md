# HIDAPI tests

`test_hidapi_input_ring.c` tests the shared queue in
[`src/core/hidapi_input_ring.h`](../core/hidapi_input_ring.h), including FIFO
ordering, overflow, resizing and allocation bounds. The `hidapi_input_ring`
CTest runs without a device when macOS or libusb is enabled. The standalone
Autotools consumer in `pkg-config/` checks installed pkg-config metadata.

## Virtual-device tests

Backend-generic HIDAPI tests that run against a **virtual HID device** so they
need no physical hardware. Shared scenarios use the public
HIDAPI API plus the small backend-agnostic `test_virtual_device` interface
(`test_virtual_device.h`) across every backend with a provider.
Backend-specific scenarios also check platform lifecycle behavior.

## Scenario protocol

Rather than injecting arbitrary input from the test (which would need
platform-specific plumbing), the virtual device has a few **pre-recorded
scenarios** baked in. The test triggers one using the ordinary public API — it
sends a *Feature report* whose first payload byte is a `TEST_VDEV_CMD_*` command
— and the device replays the matching canned *input report*. Device behavior
lives in the per-backend provider. See `test_virtual_device.h` for the shared contract (report size,
command bytes, expected payloads).

## Tests

| Test | What it exercises |
|------|-------------------|
| `test_device_io.c` | open → validate and resize the input queue → write an output report → trigger+read input reports → grow the queue and read again → close |
| `test_read_interrupt.c` | interrupt a blocking or timed read, retain queued input while interrupted, clear and resume, repeat interrupt/clear, and close from a Windows controller while the reader thread stays alive |

Each provider builds `DeviceIO_<backend>` and `ReadInterrupt_<backend>`.
Read-interrupt scenarios serialize interrupt, query and clear calls; only the
interrupt overlaps a read. A reader that misses its join deadline terminates
the test immediately, preserving its context until process exit.

## Providers

| Platform / backend | Provider | Mechanism | CI |
|--------------------|----------|-----------|----|
| Linux / hidraw | `test_virtual_device_uhid.c` | kernel `/dev/uhid` | runs in `builds.yml` (ubuntu-cmake) |
| Linux / libusb | `test_virtual_device_rawgadget.c` | `/dev/raw-gadget` + `dummy_hcd` (in a VM) | builds + self-skips in `builds.yml`; runs in the manual `libusb-vhid-test` job (in a VM) |
| Windows / winapi | `test_virtual_device_win.c` + `windows/driver/` | modified vhidmini2 UMDF2 driver | builds + self-skips in `builds.yml`; runs in the manual `win-vhid-test` job |
| macOS / darwin | `test_virtual_device_mac.c` | `IOHIDUserDevice` (IOKit) | builds + self-skips in `builds.yml` (macos-cmake); runs on a real Mac |

Whenever a virtual device cannot be created or does not enumerate, the test
returns CTest's **skip** code (77) instead of failing, so ordinary builds on any
host stay green.

### Why some providers only run in a dedicated job

Some virtual devices need privileged, out-of-band setup that isn't appropriate
for the per-push CI matrix:

* **Windows** — the vhidmini2 driver must be built, self-signed and installed
  (via `devcon`, no reboot) before the test, and removed afterwards. The
  `win-vhid-test` workflow does this end-to-end on hosted runners (built on
  `windows-2022`, whose MSBuild matches the WDK's driver build tasks, then
  installed and tested on `windows-latest`). The driver under `windows/driver/`
  is derived from Microsoft's
  vhidmini2 sample and is licensed separately under the **MS-PL** (not HIDAPI's
  license); see `windows/driver/README.md` and `windows/driver/LICENSE.txt`.
* **Linux / libusb** — needs the `raw_gadget` and `dummy_hcd` kernel modules.
  The `libusb-vhid-test` workflow uses an `ubuntu-24.04` runner and boots a
  generic guest kernel in a lightweight VM (`virtme-ng` + QEMU). Its
  `linux-modules-extra` package supplies `raw_gadget`; the workflow builds
  `dummy_hcd` against the matching kernel headers. The VM shares the host
  filesystem and runs the host-built binaries. The same approach supports
  local hosts and WSL2 without USB gadget kernel support.
* **macOS** — creating an `IOHIDUserDevice` is gated by the
  `com.apple.developer.hid.virtual.device` entitlement *and* an interactive
  Accessibility (TCC) consent prompt, neither available on a hosted runner, so
  the provider self-skips there. It is meant to actually run on a developer
  machine or a self-hosted runner where consent has been granted.

## Platforms without a provider

This harness implements no virtual-device provider for these platforms, so
they have no `DeviceIO_*` or `ReadInterrupt_*` test:

* **FreeBSD** — neither the native hidraw backend nor the libusb backend has a
  provider.
* **NetBSD / OpenBSD** — neither the NetBSD uhid backend nor the libusb backend
  has a provider.

Native CI on these systems builds HIDAPI. That is build coverage only; these
tests do not qualify those backends at runtime.

## Running locally

```sh
# Linux (hidraw via uhid)
cmake -B build -S . -DHIDAPI_WITH_TESTS=ON
cmake --build build
sudo modprobe uhid
sudo ctest --test-dir build -R '^(DeviceIO|ReadInterrupt)_hidraw$' --output-on-failure
```

On Windows/macOS configure with `-DHIDAPI_WITH_TESTS=ON` and run `ctest`; the
device-backed tests self-skip unless the corresponding virtual device has been
set up (see the dedicated workflows under `.github/workflows/`).

### Running the macOS (`darwin`) provider on a real Mac

The `DeviceIO_darwin` and `ReadInterrupt_darwin` tests cannot run on hosted CI: macOS
gates `IOHIDUserDevice` creation behind the private
`com.apple.developer.hid.virtual.device` entitlement *and* an interactive
Accessibility (TCC) consent prompt. To run it on a physical Mac you currently
need to:

1. have a paid **Apple Developer account** (US$99/year) and use it to generate a
   signing certificate plus a provisioning profile that carries the
   `com.apple.developer.hid.virtual.device` entitlement;
2. code-sign both test binaries with that entitlement; and
3. on first run, grant it **Accessibility** access under *System Settings →
   Privacy & Security → Accessibility* (answer the TCC prompt).

`sudo` does **not** help here: the entitlement and the consent prompt are
enforced by AMFI/TCC, not by file permissions, so running as root neither
supplies the entitlement nor bypasses the prompt.

> These are the known requirements rather than a verified, step-by-step recipe —
> the maintainers have not exercised this path themselves. Reports refining it
> are welcome.
