# Reproducing silent HID input-report loss in HIDAPI's libusb backend

## Issue

The libusb backend queues HID input reports in a linked list. When the queue reaches approximately 32 entries, `read_callback()` silently discards an older report with `return_data(dev, NULL, 0)`. Applications cannot distinguish a full transfer from one missing HID reports unless their application protocol has an independent integrity check.

The original implementation repeatedly traverses the queue to append at its tail; this is separate from the primary data-loss issue.

## Reproduction

Test platform: ThinkPad T400 running Linux; Abbott FreeStyle Libre 2 USB HID reader; `hidapi` Python package 0.15.0 compiled using its libusb backend. Python 3.14. The reader communicates over full-speed USB, tested on an EHCI hub.

The original libusb queue was instrumented with:
- `reports_received`: incoming reports queued
- `reports_dropped`: reports removed due to queue overflow
- `queue_max`: highest observed queue occupancy

Instrumentation used the existing receive-thread mutex and logged the results at `hid_close()`.

Results:

| Queue | Observed statistics | Result |
| --- | --- | --- |
| Original (~32), light load | received=22658, dropped=0, max_queue=4 | Complete transfer |
| Original (~32), moderate desktop activity | received=22659, dropped=0, max_queue=27 | Complete transfer |
| Original (~32), under activity | received=12365, dropped=92, max_queue=33 | Historical transfer checksum failure |
| Experimental (~1024) | received=22658, dropped=0, max_queue=88 | Complete transfer |
| Experimental (~1024), heavier activity | received=22659, dropped=0, max_queue=145 | Complete transfer |
| Experimental (~1024), end-to-end validation | received=22658, dropped=0, max_queue=84 | Complete transfer |

The failed historical transfer contained 12,269 HID reports; a complete historical transfer contained 12,361. The difference of **92 reports exactly matched the native overflow count**.

A successful end-to-end collector run was about 33 seconds, read 12,328 historical records and 5,032 scan events, and confirmed counters `received=22658 dropped=0 max_queue=84` both from Python and C upon close.

No patient information, device serial numbers, or patient-level records are included here.

## Locally validated mitigation

The validated experimental implementation changes the overflow threshold in the vendored HIDAPI 0.15.0 libusb backend from `num_queued > 30` to `num_queued > 1022` (approximately 1,024 queued reports). It also exposes queue counters through a temporary `hid_get_queue_stats()` C API and `hid.device.get_queue_stats()` Python binding.

The collector rejects the read if the native statistics API is unavailable or if `reports_dropped > 0`. Application-level checksums remain enabled. A 1,024-report queue is not a guarantee against all scheduling delays; the overflow counter is essential.

## Intended upstream contributions

- C HIDAPI: port the **experimentally validated** queue threshold and native queue statistics to the maintained libusb backend.
- Python binding (`trezor/cython-hidapi`): expose `get_queue_stats()` on `hid.device`.

The work is split across two upstream repositories; the local experimental build combines both changes. The documentation in this branch records measured behavior; it does **not** claim upstream CI or current upstream builds have been tested.

## Source of truth

The local experimental source files under `essais-hidapi/hidapi-0.15.0` on the development workstation are the reference implementation. Do not substitute a reimplementation when preparing patches.

## API and compatibility

`hid_get_queue_stats(dev, &received, &dropped, &max_queue)` returns zero
for an open libusb device. All pointers are required; invalid arguments return
-1 without changing outputs. The existing receive mutex protects the snapshot.
Other backends export the same function and return -1 without changing outputs.
`HID_API_HAS_QUEUE_STATS` identifies headers declaring this API.

The 1024-report limit preserves the tested oldest-report discard policy.
`max_queue` measures depth before overflow removal and can therefore reach 1025.
Counters are cumulative for the lifetime of an open device and do not reset
when read. Closing a libusb device logs the native counters to standard error.
Callers must serialize close against statistics reads, as for other device APIs.

## Hardware-free regression

From the repository root, compile and run sequentially:

```sh
mkdir -p build-local
cc -g -fsanitize=address,undefined -pthread -Ihidapi -Ilibusb \
  $(pkg-config --cflags libusb-1.0) tests/queue-stats/test_queue_stats.c \
  -o build-local/test-queue-stats $(pkg-config --libs libusb-1.0)
./build-local/test-queue-stats
```

The test uses the actual receive callback and queue with mocked USB initialization,
shutdown and transfer submission. It verifies invalid arguments, initial counters,
the 1024/1025 overflow boundary, FIFO retention of the newest 1024 reports,
and cumulative counters after 22658 synthetic reports. It never discovers or
opens a USB device.
