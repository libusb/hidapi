# HIDAPI virtual-device tests

Backend-generic HIDAPI tests that run against a **virtual HID device** so they
need no physical hardware. Every test is written purely against the public
HIDAPI API plus the small backend-agnostic `test_virtual_device` interface
(`test_virtual_device.h`), so the *same* test runs against every backend that
provides a virtual-device implementation.

## Scenario protocol

Rather than injecting arbitrary input from the test (which would need
platform-specific plumbing), the virtual device has a few **pre-recorded
scenarios** baked in. The test triggers one using the ordinary public API — it
sends a *Feature report* whose first payload byte is a `TEST_VDEV_CMD_*` command
— and the device replays the matching canned *input report*. This keeps the test
code 100% platform-neutral; all device behaviour lives in the per-backend
provider. See `test_virtual_device.h` for the shared contract (report size,
command bytes, expected payloads).

## Tests

| Test | What it exercises |
|------|-------------------|
| `test_device_io.c` | open → write an output report → trigger+read input reports (Feature-report write, then input-report read-back) → close |

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
* **Linux / libusb** — needs the `raw_gadget` and `dummy_hcd` kernel modules,
  which the hosted `ubuntu-latest` kernel is built *without* (it has no USB
  gadget subsystem). The `libusb-vhid-test` workflow therefore runs the test
  inside a lightweight VM (`virtme-ng` + QEMU) booting a *generic* Ubuntu kernel
  whose `linux-modules-extra` ships both modules; the VM shares the host
  filesystem, so it runs the host-built binaries. The same approach works
  locally and on WSL2 (whose default kernel also lacks these modules).
* **macOS** — creating an `IOHIDUserDevice` is gated by the
  `com.apple.developer.hid.virtual.device` entitlement *and* an interactive
  Accessibility (TCC) consent prompt, neither available on a hosted runner, so
  the provider self-skips there. It is meant to actually run on a developer
  machine or a self-hosted runner where consent has been granted.

## Platforms without a provider (documented, not implemented)

Some platforms have no practical way to *create* a virtual HID device — not even
in CI — so there is intentionally no provider for them (the backend simply has
no `DeviceIO_*` test):

* **FreeBSD** — HIDAPI uses the libusb backend, but FreeBSD has no userspace
  USB/HID *creation* facility: `cuse(3)` can't produce a libusb-visible USB
  device, `usb_template(4)` ("USB device mode") needs a hardware USB Device
  Controller (there is no `dummy_hcd` analogue), and the planned `usrhid(4)` (a
  `/dev/uhid` equivalent) is not yet in-tree. A real test would need physical
  device-mode hardware or `usrhid`.
* **NetBSD / OpenBSD** — `uhid(4)` is consumer-only (no create/emulate ioctl),
  there is no `cuse`, and `rump` has no virtual USB host controller; on OpenBSD
  the libusb backend additionally needs `uhid`/`uhidev` disabled in a custom
  kernel. No userspace virtual-device path exists.

CI can build HIDAPI on these systems, but cannot exercise a virtual device, so
the device-I/O test is not wired up there.

## Running locally

```sh
# Linux (hidraw via uhid)
cmake -B build -S . -DHIDAPI_WITH_TESTS=ON
cmake --build build
sudo modprobe uhid
sudo ctest --test-dir build -R DeviceIO_hidraw --output-on-failure
```

On Windows/macOS configure with `-DHIDAPI_WITH_TESTS=ON` and run `ctest`; the
device-backed tests self-skip unless the corresponding virtual device has been
set up (see the dedicated workflows under `.github/workflows/`).

### Running the macOS (`darwin`) provider on a real Mac

`DeviceIO_darwin` is the one virtual device that cannot run on hosted CI: macOS
gates `IOHIDUserDevice` creation behind the private
`com.apple.developer.hid.virtual.device` entitlement *and* an interactive
Accessibility (TCC) consent prompt. To run it on a physical Mac you currently
need to:

1. have a paid **Apple Developer account** (US$99/year) and use it to generate a
   signing certificate plus a provisioning profile that carries the
   `com.apple.developer.hid.virtual.device` entitlement;
2. code-sign the built `DeviceIO_darwin` binary with that entitlement; and
3. on first run, grant it **Accessibility** access under *System Settings →
   Privacy & Security → Accessibility* (answer the TCC prompt).

`sudo` does **not** help here: the entitlement and the consent prompt are
enforced by AMFI/TCC, not by file permissions, so running as root neither
supplies the entitlement nor bypasses the prompt.

> These are the known requirements rather than a verified, step-by-step recipe —
> the maintainers have not exercised this path themselves. Reports refining it
> are welcome.
