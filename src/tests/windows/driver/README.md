# Vendored vhidmini2 UMDF2 driver (HIDAPI test fixture)

This directory contains a small **virtual HID minidriver** used only by the
HIDAPI virtual-device tests on Windows (the `winapi` backend's
`DeviceIO_winapi` test). It is **not** part of the HIDAPI library: it is a
standalone UMDF 2 driver that the `win-vhid-test` CI job builds, self-signs and
installs out-of-band, runs the test against, then removes.

## Provenance

These files are **derived from the `hid/vhidmini2` sample** in Microsoft's
[`microsoft/Windows-driver-samples`](https://github.com/microsoft/Windows-driver-samples)
repository (the UMDF 2 "VhidminiUm" variant).

## License

The upstream sample repository is licensed under the **Microsoft Public License
(MS-PL)**. A complete, verbatim copy of that license is included here as
[`LICENSE.txt`](./LICENSE.txt), and the original Microsoft copyright notices are
retained in each file's header. (Those per-file headers carry Microsoft's older
sample boilerplate — "All Rights Reserved" / "THIS CODE … IS PROVIDED 'AS IS'" —
but the governing license for the sample is the repository-level MS-PL.)

**The files in this directory remain under the MS-PL**, which is *separate* from
HIDAPI's own licensing. HIDAPI itself is offered under your choice of the GNU
GPL v3, a BSD-style license, or the original HIDAPI license (see the
`LICENSE*.txt` files at the root of this repository); that choice does **not**
apply to the MS-PL files here.

MS-PL is not GPL-compatible, but this driver is a **separate program**, not a
derivative or a linked part of the HIDAPI library: it is built independently and
communicates with HIDAPI only at runtime, through the Windows HID stack. Its
presence in the source tree is therefore mere aggregation, not a combined work.

## What was modified

| File | Status |
|------|--------|
| `vhidmini.c` | **Modified** — default report descriptor matches the Linux uhid test device byte-for-byte; implements the HIDAPI pre-recorded "scenario" protocol (a Feature `SET_REPORT` command makes the device replay a canned input report; see `../../test_virtual_device.h`). |
| `vhidmini.h` | **Modified** — supporting declarations for the scenario protocol. |
| `common.h`, `util.c`, `vhidmini.rc`, `VhidminiUm.inx`, `VhidminiUm.vcxproj` | Used essentially as-is (no HIDAPI-specific changes beyond what's needed to build the standalone `VhidminiUm.dll`). |
