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
| `test_hotplug_api.c` | tier-1 hotplug API contract, no device needed: argument validation, handle properties, implicit init, `hid_exit()` teardown, register/deregister thread churn |
| `test_hotplug.c` | tier-2 hotplug scenarios against a virtual device whose presence is toggled: async delivery, event masks, exactly-once ENUMERATE pass, callback-return deregistration, pass-before-live ordering, full payloads, filtering, dispatch order, deregistration post-condition, re-entrant registration and callback error isolation |

## Hotplug tests

The hotplug tests come in two tiers:

* **Tier 1 — `HotplugAPI_<backend>`** (`test_hotplug_api.c`): the parts of the
  hotplug contract observable *without* a device event: argument validation,
  callback-handle properties and stale-handle safety, implicit `hid_init()`,
  `hid_exit()` teardown (including the register-to-immediate-`hid_exit()` loop),
  and two-thread register/deregister churn. Needs no virtual device or
  privileges, so it runs against **every** backend in the ordinary per-push CI
  matrix. If the first registration fails, the probe retries after explicit
  `hid_init()`: a successful retry fails the test for broken implicit
  initialization; it self-skips (77) only when the retry also fails, including
  a backend that reports hotplug as unsupported at runtime (e.g. a libusb
  without `LIBUSB_CAP_HAS_HOTPLUG`).
* **Tier 2 — `Hotplug_<backend>`** (`test_hotplug.c`): device-backed hotplug
  scenarios. On top of a virtual device, the provider must be able to *toggle
  the device's presence* (`test_virtual_device_unplug()` /
  `test_virtual_device_replug()` in `test_virtual_device.h`). The **uhid**
  (`UHID_DESTROY` / `UHID_CREATE2` on the same open `/dev/uhid` fd),
  **rawgadget** (unbind/rebind the gadget from the `dummy_hcd` UDC) and
  Windows **vhidmini** (disable/enable the HID child devnode) providers all
  implement toggling. `Hotplug_hidraw` runs per-push (in `builds.yml`'s
  ubuntu-cmake job, like `DeviceIO_hidraw`); `Hotplug_libusb` and
  `Hotplug_winapi` run in the label-gated `ci-virtual-device` jobs
  (`libusb-vhid-test` / `win-vhid-test`), which provide the privileged
  environment those providers need. The darwin provider still returns
  `TEST_VDEV_UNAVAILABLE` from the toggle calls, so `Hotplug_darwin`
  self-skips until presence toggling is implemented for it.

  T8b, cancellation during an ENUMERATE pass, needs two concurrent devices and
  is reported as a distinct skipped subtest for the single-device Raw Gadget and
  Windows providers; the UHID provider exercises it.

| Test | Runs per-push in `builds.yml` | Notes |
|------|-------------------------------|-------|
| `HotplugAPI_hidraw` | yes (ubuntu-cmake) | |
| `HotplugAPI_libusb` | yes (ubuntu-cmake) | needs libusb hotplug support at runtime |
| `HotplugAPI_winapi` | yes (windows-cmake-msvc: MSVC/NMake/ClangCL; windows-cmake-mingw) | |
| `HotplugAPI_darwin` | yes (macos-cmake) | |
| `Hotplug_hidraw` | yes (ubuntu-cmake, via `uhid`) | the tier-2 test that runs per-push |
| `Hotplug_libusb` | builds, self-skips | runs in the label-gated `libusb-vhid-test` VM job |
| `Hotplug_winapi` | builds, self-skips | runs in the label-gated `win-vhid-test` job |
| `Hotplug_darwin` | builds, self-skips | needs `IOHIDUserDevice` re-creation (future) |

The tier-2 test is written against strict synchronization rules (hotplug tests
are notoriously flaky otherwise): recording callbacks only deep-copy the event
into a log under a lock and never call `hid_enumerate`, `hid_open`, or
`hid_error(NULL)`; T14 deliberately holds a callback open so deregistration's
wait is observable, and T15 registers/deregisters from a callback as the API
allows. Every expectation is awaited with a deadline-based predicate poll; the
*absence* of an event is asserted behind an
**event barrier** — a later event that is provably ordered after the missing
one — never behind a time window; and a missed event within the (generous)
budget is treated as a bug, not retried.

T9b holds a snapshot callback while the device disconnects; T18/T18b exercise
immediate and queued-snapshot cancellation, and T19 exits with ENUMERATE work
before reinitializing. Set `HIDAPI_HOTPLUG_STRESS=1` to additionally run T20's
25 arrival-versus-snapshot races. After an event deadline fails, the suite
cleans up and prints its summary without starting another scenario.

## Providers

| Platform / backend | Provider | Mechanism | CI |
|--------------------|----------|-----------|----|
| Linux / hidraw | `test_virtual_device_uhid.c` | kernel `/dev/uhid` | runs in `builds.yml` (ubuntu-cmake) |
| Linux / libusb | `test_virtual_device_rawgadget.c` | `/dev/raw-gadget` + `dummy_hcd` (in a VM) | builds + self-skips in `builds.yml`; runs in `libusb-vhid-test` (workflow dispatch or the `ci-virtual-device` PR label), in a VM |
| Windows / winapi | `test_virtual_device_win.c` + `windows/driver/` | modified vhidmini2 UMDF2 driver | builds + self-skips in `builds.yml`; runs in `win-vhid-test` (workflow dispatch or the `ci-virtual-device` PR label) |
| macOS / darwin | `test_virtual_device_mac.c` | `IOHIDUserDevice` (IOKit) | builds + self-skips in `builds.yml` (macos-cmake); runs on a real Mac |

Whenever a virtual device cannot initially be created or does not initially enumerate, the test
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
  inside a lightweight VM (`virtme-ng` + QEMU) booting a *generic* Ubuntu kernel:
  `linux-modules-extra` supplies `raw_gadget`; Ubuntu does not package
  `dummy_hcd`, so the workflow builds it from matching upstream kernel source
  against that kernel's headers and installs it alongside. The VM shares the host
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
cd build
sudo ctest -R DeviceIO_hidraw --output-on-failure
sudo ctest -R Hotplug_hidraw --output-on-failure
# tier-1 hotplug API tests need no device and no root:
ctest -R HotplugAPI --output-on-failure
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
