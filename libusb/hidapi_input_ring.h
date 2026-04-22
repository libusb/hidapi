/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 Internal ring buffer — a bounded FIFO of variable-length byte
 buffers, with drop-oldest-when-full semantics. Used by the mac
 (IOKit) and libusb hidapi backends to queue input reports produced
 asynchronously by the device's delivery path, until the caller
 drains them via hid_read() / hid_read_timeout().

 All helpers are defined as `static` so each translation unit that
 includes this header gets its own private copy — no symbols are
 exported from the shared library.

 ---- Lifecycle ----
 hidapi_input_ring_init(r, CAPACITY) is called once at ring-owner open
 (for input-report use: at hid_open(), with CAPACITY =
 HID_API_MAX_NUM_INPUT_BUFFERS). The backing array is sized to the
 absolute maximum capacity and never reallocated for the ring's
 lifetime — the only runtime knob is the LOGICAL cap (drop-oldest
 threshold) passed into hidapi_input_ring_push() per call. For input
 reports the logical cap lives in dev->num_input_buffers.

 Preconditions for init(): `r->slots` must be NULL (never-initialized
 or previously-destroyed state). Callers typically embed `struct
 hidapi_input_ring` inside a struct allocated by `calloc`, which
 zeros `r->slots` and satisfies this automatically. Other fields
 may hold garbage — init overwrites them.

 ---- Concurrency ----
 EVERY helper here requires the caller to hold a mutex protecting
 the ring. The helpers do not lock internally. For input-report
 rings that mutex is the device's queue mutex (pthread_mutex_t on
 mac, hidapi_thread_state on libusb); for other uses, the caller
 chooses.

 ---- Ownership ----
 Each slot owns its data allocation (malloc'd in push, freed on
 evict / pop / destroy). Zero-length reports are stored with
 .data == NULL and .len == 0 — no allocation occurs.
 *******************************************************/

#ifndef HIDAPI_INPUT_RING_H_
#define HIDAPI_INPUT_RING_H_

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct hidapi_input_ring_slot {
    uint8_t *data;
    size_t   len;
};

struct hidapi_input_ring {
    struct hidapi_input_ring_slot *slots;
    int      capacity;    /* allocated slot count; fixed after init    */
    int      head;        /* oldest report index (dequeue side)        */
    int      tail;        /* next free slot index (enqueue side)       */
    int      count;       /* valid reports currently queued            */
    uint64_t dropped;     /* reports dropped by queue policy           */
                          /* (cap evictions + shrinking setter).       */
                          /* Does not count ENOMEM.                    */
};

/* PRECONDITION: r must be in the never-initialized or destroyed
 *               state (r->slots == NULL). Other fields may hold
 *               garbage — init overwrites them. Re-init on an
 *               already-initialized ring is rejected to prevent
 *               leaking the previous allocation.
 * POSTCONDITION on success: r is empty with `capacity` slots allocated.
 * Returns 0 on success, -1 on invalid arg, double-init, or ENOMEM. */
static int hidapi_input_ring_init(struct hidapi_input_ring *r, int capacity)
{
    if (!r || capacity < 1)
        return -1;
    if (r->slots)       /* double-init guard — prevents leak */
        return -1;
    r->slots = (struct hidapi_input_ring_slot *)
        calloc((size_t)capacity, sizeof(struct hidapi_input_ring_slot));
    if (!r->slots)
        return -1;
    r->capacity = capacity;
    r->head = 0;
    r->tail = 0;
    r->count = 0;
    r->dropped = 0;
    return 0;
}

/* PRECONDITION: caller holds the device's queue mutex, OR is tearing
 *               down the device such that no other thread can reach r.
 * POSTCONDITION: r is zeroed. All queued report data is freed.
 *
 * Safe to call on a zero-initialized or previously-destroyed ring.
 * Not safe on uninitialized stack memory — callers must zero-init
 * the struct before first use (see init() precondition). */
static void hidapi_input_ring_destroy(struct hidapi_input_ring *r)
{
    if (!r) return;
    if (r->slots) {
        int idx = r->head;
        for (int i = 0; i < r->count; i++) {
            free(r->slots[idx].data);
            r->slots[idx].data = NULL;
            r->slots[idx].len = 0;
            idx = (idx + 1) % r->capacity;
        }
        free(r->slots);
    }
    r->slots = NULL;
    r->capacity = 0;
    r->count = 0;
    r->head = 0;
    r->tail = 0;
    r->dropped = 0;
}

/* Enqueue a copy of [data, data+len). Evicts oldest reports while
 * r->count >= logical_cap, incrementing r->dropped per evicted report.
 * For len == 0, stores (NULL, 0) — no allocation.
 *
 * PRECONDITION: caller holds the mutex protecting r.
 *               1 <= logical_cap <= r->capacity.
 *               If len > 0, data must not be NULL.
 *
 * Returns 0 on success, -1 on invalid args or ENOMEM. On ENOMEM the
 * ring is unchanged and dropped is NOT incremented. */
static int hidapi_input_ring_push(struct hidapi_input_ring *r, int logical_cap,
                                  const uint8_t *data, size_t len)
{
    if (!r || !r->slots || logical_cap < 1 || logical_cap > r->capacity ||
        (len > 0 && !data))       /* NULL-data guard (UB in memcpy otherwise) */
        return -1;

    /* Allocate BEFORE touching ring state so ENOMEM leaves r unchanged.
     * For zero-length reports we skip allocation; free(NULL) is safe
     * and pop() distinguishes via the slot's .len field. */
    uint8_t *copy = NULL;
    if (len > 0) {
        copy = (uint8_t *)malloc(len);
        if (!copy)
            return -1;   /* ENOMEM — ring unchanged, dropped NOT incremented */
        memcpy(copy, data, len);
    }

    /* Evict oldest while at or over the logical cap. Terminates because
     * logical_cap >= 1 (precondition) and count decreases each iter. */
    while (r->count >= logical_cap) {
        free(r->slots[r->head].data);
        r->slots[r->head].data = NULL;
        r->slots[r->head].len = 0;
        r->head = (r->head + 1) % r->capacity;
        r->count--;
        r->dropped++;
    }

    r->slots[r->tail].data = copy;
    r->slots[r->tail].len  = len;
    r->tail = (r->tail + 1) % r->capacity;
    r->count++;
    return 0;
}

/* Remove the oldest report into (*out_data, *out_len).
 * Caller owns the returned *out_data and must free() it
 * (free(NULL) is safe and expected for zero-length reports).
 *
 * PRECONDITION: caller holds the mutex protecting r.
 *
 * Returns 0 on success, -1 if empty or on invalid args. */
static int hidapi_input_ring_pop(struct hidapi_input_ring *r,
                                 uint8_t **out_data, size_t *out_len)
{
    if (!r || !r->slots || !out_data || !out_len)
        return -1;
    if (r->count == 0)
        return -1;
    *out_data = r->slots[r->head].data;
    *out_len  = r->slots[r->head].len;
    r->slots[r->head].data = NULL;
    r->slots[r->head].len  = 0;
    r->head = (r->head + 1) % r->capacity;
    r->count--;
    return 0;
}

/* Drop the N oldest reports. Used by hid_set_num_input_buffers() when
 * shrinking the logical cap. Increments r->dropped by N.
 *
 * PRECONDITION: caller holds the mutex protecting r.
 *               0 <= n <= r->count.
 *
 * Invalid n (out of range) is silently ignored — matches the project
 * convention of not using runtime asserts in backend code. Callers
 * must meet the precondition; violations are caller bugs. */
static void hidapi_input_ring_drop_oldest(struct hidapi_input_ring *r, int n)
{
    if (!r || !r->slots || n < 0 || n > r->count)
        return;

    for (int i = 0; i < n; i++) {
        free(r->slots[r->head].data);
        r->slots[r->head].data = NULL;
        r->slots[r->head].len  = 0;
        r->head = (r->head + 1) % r->capacity;
        r->count--;
        r->dropped++;
    }
}

#endif /* HIDAPI_INPUT_RING_H_ */
