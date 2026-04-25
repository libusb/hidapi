/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 Internal ring buffer — a bounded FIFO of HID input reports.
 Storage is a single pre-allocated flat byte buffer of
 (capacity × slot_size) bytes, where slot_size is a backend-specific
 runtime upper bound on input report size, determined at open time.
 Drop-oldest-when-full semantics.

 All helpers are defined as `static`; each translation unit
 that includes this header gets its own private copy.

 ---- Lifecycle ----
 hidapi_input_ring_init(r, capacity, slot_size) is called by each
 backend once its runtime upper bound on input report size is known:
   - macOS: kIOHIDMaxInputReportSizeKey, an authoritative value
            derived from the HID report descriptor by IOKit
   - libusb: wMaxPacketSize of the interrupt-IN endpoint — an upper
             bound consistent with hidapi's libusb backend, which
             only transfers single packets
 capacity comes from dev->num_input_buffers (default 30).
 The slot array is sized exactly, never to MAX.

 hidapi_input_ring_resize() grows or shrinks the allocation under
 the caller-held mutex when hid_set_num_input_buffers() is called.
 r->slot_size is fixed for the ring's lifetime; only r->capacity
 and (on shrink-below-count) r->count change.

 ---- Concurrency ----
 EVERY helper requires the caller to hold the mutex protecting r.
 The helpers do not lock internally.

 ---- Ownership ----
 The ring owns its entire backing storage throughout its lifetime. No
 pointer into that storage is ever exposed to callers. Pop copies the
 oldest report's bytes into a caller-supplied buffer and advances the
 ring head — no borrowed-pointer contract to misuse, no slot lifetime
 tied to subsequent ring operations. Push is allocation-free: memcpy
 into the next slot's inline bytes.

 Note: after pop_into() advances r->head, the length entry for the
 just-vacated slot is left stale in r->lengths[]. This is harmless —
 r->lengths[i] is only read when slot i is the current r->head, which
 only happens again after a push has overwritten both the storage
 bytes AND the length entry for that slot.
 *******************************************************/

#ifndef HIDAPI_INPUT_RING_H_
#define HIDAPI_INPUT_RING_H_

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct hidapi_input_ring {
    uint8_t *storage;     /* capacity × slot_size bytes; one allocation */
    size_t  *lengths;     /* actual len of each queued report; capacity entries */
    int      capacity;    /* slot count; changed only by resize()   */
    size_t   slot_size;   /* bytes per slot; fixed at init          */
    int      head;        /* oldest report index (dequeue side)     */
    int      tail;        /* next free slot index (enqueue side)    */
    int      count;       /* valid reports currently queued         */
    uint64_t dropped;     /* reports dropped by queue policy
                             (push-time evictions + resize shrink drops).
                             Does not count ENOMEM. */
};

/* PRECONDITION: r->storage == NULL (never-initialized or destroyed).
 * Returns 0 on success; -1 on invalid arg, double-init, or ENOMEM. */
static int hidapi_input_ring_init(struct hidapi_input_ring *r,
                                  int capacity, size_t slot_size)
{
    if (!r || capacity < 1 || slot_size < 1)
        return -1;
    if (r->storage)      /* double-init guard */
        return -1;
    /* Reject capacity × slot_size that would overflow size_t. */
    if ((size_t)capacity > SIZE_MAX / slot_size)
        return -1;

    r->storage = (uint8_t *)malloc((size_t)capacity * slot_size);
    r->lengths = (size_t *)calloc((size_t)capacity, sizeof(size_t));
    if (!r->storage || !r->lengths) {
        free(r->storage);
        free(r->lengths);
        r->storage = NULL;
        r->lengths = NULL;
        return -1;
    }
    r->capacity  = capacity;
    r->slot_size = slot_size;
    r->head = 0;
    r->tail = 0;
    r->count = 0;
    r->dropped = 0;
    return 0;
}

/* PRECONDITION: caller holds r's mutex, or is tearing down.
 * Frees storage and lengths; zeros r. Safe on zero-init or
 * previously-destroyed rings. */
static void hidapi_input_ring_destroy(struct hidapi_input_ring *r)
{
    if (!r) return;
    free(r->storage);
    free(r->lengths);
    r->storage   = NULL;
    r->lengths   = NULL;
    r->capacity  = 0;
    r->slot_size = 0;
    r->head = 0;
    r->tail = 0;
    r->count = 0;
    r->dropped = 0;
}

/* Enqueue a copy of [data, data+len). memcpy only — no malloc.
 * Evicts oldest when count >= capacity. len == 0 stores a zero-length
 * report (lengths[tail]=0, no memcpy).
 *
 * PRECONDITION: caller holds r's mutex; if len > 0, data != NULL.
 *
 * Returns 0; -1 on invalid args or len > slot_size. On failure r is
 * unchanged and dropped is NOT incremented. */
static int hidapi_input_ring_push(struct hidapi_input_ring *r,
                                  const uint8_t *data, size_t len)
{
    if (!r || !r->storage || (len > 0 && !data))
        return -1;
    if (len > r->slot_size)
        return -1;   /* oversized report — reject, don't truncate */

    if (r->count >= r->capacity) {
        /* Drop oldest: just advance head. No per-payload free needed. */
        r->head = (r->head + 1) % r->capacity;
        r->count--;
        r->dropped++;
    }

    if (len > 0) {
        memcpy(r->storage + (size_t)r->tail * r->slot_size, data, len);
    }
    r->lengths[r->tail] = len;
    r->tail = (r->tail + 1) % r->capacity;
    r->count++;
    return 0;
}

/* Copy the oldest report's bytes into [dst, dst + dst_len). If the
 * report payload is larger than dst_len, it's truncated to dst_len
 * bytes. The report is consumed from the ring regardless.
 *
 * Copy-out semantics — no borrowed pointer is ever exposed outside
 * this helper. This keeps the slot lifetime entirely contained: the
 * caller's buffer owns the bytes after return, and the ring is free
 * to reuse the vacated slot on the next push.
 *
 * PRECONDITION: caller holds r's mutex.
 *               If dst_len > 0, dst != NULL.
 *
 * Edge case: pop_into(r, NULL, 0) is valid and consumes the head
 * slot without copying (to_copy == 0, no memcpy, head++, count--).
 * This mirrors the pre-PR ring_pop_into() helper's own behavior at
 * length=0. Note: hid_read_timeout() only reaches this code path on
 * libusb (mac's hid_read_timeout has an early length==0 guard that
 * fails with "Zero buffer/length" before the ring is touched).
 *
 * Returns: bytes copied (0 <= rc <= dst_len) on success;
 *          -1 if empty ring or invalid args (including dst_len>0 with dst=NULL). */
static int hidapi_input_ring_pop_into(struct hidapi_input_ring *r,
                                      uint8_t *dst, size_t dst_len)
{
    if (!r || !r->storage)
        return -1;
    if (dst_len > 0 && !dst)
        return -1;
    if (r->count == 0)
        return -1;

    size_t payload_len = r->lengths[r->head];
    size_t to_copy = (payload_len < dst_len) ? payload_len : dst_len;
    if (to_copy > 0) {
        memcpy(dst, r->storage + (size_t)r->head * r->slot_size, to_copy);
    }
    r->head = (r->head + 1) % r->capacity;
    r->count--;
    return (int)to_copy;
}

/* Resize the slot array to new_cap. Allocates a new flat storage buffer
 * and lengths array, memcpy's surviving reports' bytes into the new
 * storage preserving FIFO order, then frees the old buffers.
 *
 * On shrink below count, the oldest (count - new_cap) reports are
 * evicted — matching the push-time drop-oldest policy.
 *
 * slot_size is preserved (fixed for the ring's lifetime).
 *
 * PRECONDITION: caller holds r's mutex; new_cap >= 1.
 * On failure r is unchanged.
 *
 * Returns 0; -1 on invalid args or ENOMEM. */
static int hidapi_input_ring_resize(struct hidapi_input_ring *r, int new_cap)
{
    if (!r || !r->storage || new_cap < 1)
        return -1;
    if (new_cap == r->capacity)
        return 0;
    /* Reject new_cap × slot_size that would overflow size_t. */
    if ((size_t)new_cap > SIZE_MAX / r->slot_size)
        return -1;

    uint8_t *new_storage = (uint8_t *)malloc((size_t)new_cap * r->slot_size);
    size_t  *new_lengths = (size_t *)calloc((size_t)new_cap, sizeof(size_t));
    if (!new_storage || !new_lengths) {
        free(new_storage);
        free(new_lengths);
        return -1;
    }

    int keep    = (r->count > new_cap) ? new_cap : r->count;
    int dropped = r->count - keep;

    /* Copy surviving reports' payload bytes into new storage, packed
     * starting at slot 0. Oldest dropped entries are simply not copied. */
    int src = (r->head + dropped) % r->capacity;
    for (int dst = 0; dst < keep; dst++) {
        if (r->lengths[src] > 0) {
            memcpy(new_storage + (size_t)dst * r->slot_size,
                   r->storage  + (size_t)src * r->slot_size,
                   r->lengths[src]);
        }
        new_lengths[dst] = r->lengths[src];
        src = (src + 1) % r->capacity;
    }

    r->dropped += dropped;

    free(r->storage);
    free(r->lengths);

    r->storage  = new_storage;
    r->lengths  = new_lengths;
    r->capacity = new_cap;
    r->head     = 0;
    r->count    = keep;
    r->tail     = keep % new_cap;   /* wraps to 0 when keep == new_cap */

    return 0;
}

#endif /* HIDAPI_INPUT_RING_H_ */
