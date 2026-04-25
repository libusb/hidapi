/* Unit tests for core/hidapi_input_ring.h (flat pre-alloc design).
 * Backend-agnostic — includes the header directly.
 */

#include <stdio.h>
#include <string.h>

#include "hidapi_input_ring.h"

#define SLOT_SZ 64   /* plausible HID report size */

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static int test_init_destroy(void) {
    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 4, SLOT_SZ) == 0);
    CHECK(r.capacity == 4 && r.slot_size == SLOT_SZ && r.count == 0);
    CHECK(r.head == 0 && r.tail == 0);
    hidapi_input_ring_destroy(&r);
    CHECK(r.storage == NULL && r.capacity == 0 && r.slot_size == 0);
    return 0;
}

static int test_init_invalid(void) {
    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 0, SLOT_SZ)   == -1);
    CHECK(hidapi_input_ring_init(&r, -1, SLOT_SZ)  == -1);
    CHECK(hidapi_input_ring_init(&r, 4, 0)         == -1);   /* slot_size=0 */
    CHECK(hidapi_input_ring_init(NULL, 4, SLOT_SZ) == -1);
    CHECK(hidapi_input_ring_init(&r, 4, SLOT_SZ)   == 0);
    CHECK(hidapi_input_ring_init(&r, 4, SLOT_SZ)   == -1);   /* double-init */
    hidapi_input_ring_destroy(&r);
    return 0;
}

static int test_destroy_idempotent(void) {
    hidapi_input_ring_destroy(NULL);

    struct hidapi_input_ring zero = {0};
    hidapi_input_ring_destroy(&zero);
    CHECK(zero.storage == NULL && zero.capacity == 0);

    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 4, SLOT_SZ) == 0);
    hidapi_input_ring_destroy(&r);
    hidapi_input_ring_destroy(&r);
    CHECK(r.storage == NULL);
    return 0;
}

static int test_invalid_args(void) {
    /* NULL / uninit / bad-arg rejections on push and pop_into. */
    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 4, SLOT_SZ) == 0);

    uint8_t v[4] = {1,2,3,4};
    uint8_t buf[SLOT_SZ];
    struct hidapi_input_ring uninit = {0};

    /* push */
    CHECK(hidapi_input_ring_push(NULL, v, 4)      == -1);
    CHECK(hidapi_input_ring_push(&r, NULL, 4)     == -1);
    CHECK(hidapi_input_ring_push(&uninit, v, 4)   == -1);

    /* pop_into */
    CHECK(hidapi_input_ring_pop_into(NULL, buf, sizeof(buf))    == -1);
    CHECK(hidapi_input_ring_pop_into(&r, NULL, sizeof(buf))     == -1);   /* dst_len>0, dst=NULL */
    CHECK(hidapi_input_ring_pop_into(&uninit, buf, sizeof(buf)) == -1);

    CHECK(hidapi_input_ring_push(&r, v, 4) == 0);   /* ring still usable */
    hidapi_input_ring_destroy(&r);
    return 0;
}

static int test_push_oversized_report(void) {
    /* len > slot_size must be rejected without consuming a slot. */
    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 4, 8) == 0);   /* slot_size=8 */

    uint8_t small[8] = {0};
    uint8_t big[9]   = {0};

    CHECK(hidapi_input_ring_push(&r, small, 8) == 0);   /* fits exactly */
    CHECK(hidapi_input_ring_push(&r, big,   9) == -1);  /* overflow → reject */
    CHECK(r.count == 1 && r.dropped == 0);

    hidapi_input_ring_destroy(&r);
    return 0;
}

static int test_init_overflow_reject(void) {
    /* capacity × slot_size must not overflow size_t. Guards:
     *   init:   (size_t)capacity > SIZE_MAX / slot_size
     *   resize: (size_t)new_cap  > SIZE_MAX / r->slot_size
     * Defensive — in practice slot_size comes from backend descriptors
     * capped well below SIZE_MAX, but a misreporting or fuzzed path
     * could otherwise drive a wrapped allocation size into malloc. */
    struct hidapi_input_ring r = {0};

    /* init guard: 2 × SIZE_MAX wraps. */
    CHECK(hidapi_input_ring_init(&r, 2, SIZE_MAX) == -1);
    /* r left untouched on failure — all fields still zero-initialized. */
    CHECK(r.storage == NULL);
    CHECK(r.lengths == NULL);
    CHECK(r.capacity == 0);
    CHECK(r.slot_size == 0);
    CHECK(r.count == 0 && r.head == 0 && r.tail == 0 && r.dropped == 0);

    /* init now succeeds with sane values. */
    CHECK(hidapi_input_ring_init(&r, 4, SLOT_SZ) == 0);

    /* resize guard: the expression is identical in shape to init's, so
     * exercising it from a valid ring is sufficient evidence. The guard
     * can only fire with a slot_size far larger than typical HID reports
     * (> SIZE_MAX / INT_MAX bytes); at realistic slot sizes the guard is
     * unreachable via any int new_cap, so this test temporarily inflates
     * r.slot_size to exercise the branch. slot_size is restored before
     * destroy to keep ring state consistent. */
    size_t saved_slot_size = r.slot_size;
    r.slot_size = SIZE_MAX / 2;
    CHECK(hidapi_input_ring_resize(&r, 3) == -1);
    r.slot_size = saved_slot_size;

    hidapi_input_ring_destroy(&r);
    return 0;
}

static int test_pop_into_truncation(void) {
    /* When dst_len < payload_len, pop_into copies min(payload_len, dst_len)
     * bytes and consumes the slot. Return value is bytes copied (not
     * original payload size). */
    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 4, SLOT_SZ) == 0);

    uint8_t payload[32];
    for (int i = 0; i < 32; i++) payload[i] = (uint8_t)(i + 1);
    CHECK(hidapi_input_ring_push(&r, payload, 32) == 0);

    uint8_t small_buf[10];
    CHECK(hidapi_input_ring_pop_into(&r, small_buf, sizeof(small_buf)) == 10);
    CHECK(memcmp(small_buf, payload, 10) == 0);
    CHECK(r.count == 0);   /* slot consumed even though payload was truncated */

    hidapi_input_ring_destroy(&r);
    return 0;
}

static int test_pop_into_null_zero(void) {
    /* pop_into(r, NULL, 0) is valid: consumes head slot without copying,
     * returns 0. Mirrors pre-PR ring_pop_into() helper semantics at
     * length=0. Reachable via hid_read_timeout only on libusb; mac's
     * hid_read_timeout has an early length==0 guard that returns before
     * touching the ring. */
    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 4, SLOT_SZ) == 0);

    uint8_t payload[8] = {1,2,3,4,5,6,7,8};
    CHECK(hidapi_input_ring_push(&r, payload, 8) == 0);
    CHECK(r.count == 1);

    CHECK(hidapi_input_ring_pop_into(&r, NULL, 0) == 0);
    CHECK(r.count == 0);   /* slot consumed */

    hidapi_input_ring_destroy(&r);
    return 0;
}

static int test_push_pop_fifo(void) {
    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 4, SLOT_SZ) == 0);

    uint8_t a[] = {1,2,3}, b[] = {4,5,6,7}, c[] = {8};
    CHECK(hidapi_input_ring_push(&r, a, 3) == 0);
    CHECK(hidapi_input_ring_push(&r, b, 4) == 0);
    CHECK(hidapi_input_ring_push(&r, c, 1) == 0);
    CHECK(r.count == 3 && r.dropped == 0);

    uint8_t buf[SLOT_SZ];
    CHECK(hidapi_input_ring_pop_into(&r, buf, sizeof(buf)) == 3 && memcmp(buf, a, 3) == 0);
    CHECK(hidapi_input_ring_pop_into(&r, buf, sizeof(buf)) == 4 && memcmp(buf, b, 4) == 0);
    CHECK(hidapi_input_ring_pop_into(&r, buf, sizeof(buf)) == 1 && memcmp(buf, c, 1) == 0);
    CHECK(hidapi_input_ring_pop_into(&r, buf, sizeof(buf)) == -1);
    hidapi_input_ring_destroy(&r);
    return 0;
}

static int test_drop_oldest_on_full(void) {
    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 4, SLOT_SZ) == 0);
    uint8_t v[1];
    for (int i = 0; i < 6; i++) { v[0] = (uint8_t)i; hidapi_input_ring_push(&r, v, 1); }
    CHECK(r.count == 4 && r.dropped == 2);
    uint8_t buf[SLOT_SZ];
    for (int expect = 2; expect <= 5; expect++) {
        CHECK(hidapi_input_ring_pop_into(&r, buf, sizeof(buf)) == 1 && buf[0] == (uint8_t)expect);
    }
    hidapi_input_ring_destroy(&r);
    return 0;
}

static int test_drop_oldest_varying_lengths(void) {
    /* Push reports of different sizes, trigger drop-oldest on a full ring,
     * then verify the survivors' lengths are correct. This guards against
     * a bug where push forgets to update lengths[tail] and the pop reads
     * a stale length from the previously-evicted payload.
     *
     * cap=2 to force drop-oldest on the 3rd push.
     */
    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 2, SLOT_SZ) == 0);

    uint8_t big[50]; memset(big, 0xAA, 50);
    uint8_t med[20]; memset(med, 0xBB, 20);
    uint8_t small[3] = {0xC1, 0xC2, 0xC3};

    CHECK(hidapi_input_ring_push(&r, big, 50) == 0);    /* slot 0: 50 B */
    CHECK(hidapi_input_ring_push(&r, med, 20) == 0);    /* slot 1: 20 B, full */
    /* Third push evicts big (oldest) and reuses slot 0 for small (3 B).
     * lengths[0] must now be 3, not 50. */
    CHECK(hidapi_input_ring_push(&r, small, 3) == 0);
    CHECK(r.count == 2 && r.dropped == 1);

    uint8_t buf[SLOT_SZ];
    /* First pop: med (20 B) from slot 1 */
    CHECK(hidapi_input_ring_pop_into(&r, buf, sizeof(buf)) == 20);
    CHECK(memcmp(buf, med, 20) == 0);
    /* Second pop: small (3 B) from slot 0 — lengths[0] must be 3, not stale 50 */
    CHECK(hidapi_input_ring_pop_into(&r, buf, sizeof(buf)) == 3);
    CHECK(memcmp(buf, small, 3) == 0);

    hidapi_input_ring_destroy(&r);
    return 0;
}

static int test_zero_length_report(void) {
    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 4, SLOT_SZ) == 0);
    CHECK(hidapi_input_ring_push(&r, NULL, 0) == 0);
    uint8_t buf[SLOT_SZ];
    CHECK(hidapi_input_ring_pop_into(&r, buf, sizeof(buf)) == 0);   /* 0 bytes copied, report consumed */
    CHECK(r.count == 0);
    hidapi_input_ring_destroy(&r);
    return 0;
}

static int test_resize_grow(void) {
    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 4, SLOT_SZ) == 0);
    uint8_t v[1];
    for (int i = 0; i < 2; i++) { v[0] = (uint8_t)i; hidapi_input_ring_push(&r, v, 1); }
    CHECK(hidapi_input_ring_resize(&r, 8) == 0);
    CHECK(r.capacity == 8 && r.slot_size == SLOT_SZ && r.count == 2 && r.head == 0 && r.tail == 2);
    uint8_t buf[SLOT_SZ];
    CHECK(hidapi_input_ring_pop_into(&r, buf, sizeof(buf)) == 1 && buf[0] == 0);
    CHECK(hidapi_input_ring_pop_into(&r, buf, sizeof(buf)) == 1 && buf[0] == 1);
    hidapi_input_ring_destroy(&r);
    return 0;
}

static int test_resize_shrink_below_count(void) {
    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 8, SLOT_SZ) == 0);
    uint8_t v[1];
    for (int i = 0; i < 6; i++) { v[0] = (uint8_t)i; hidapi_input_ring_push(&r, v, 1); }
    CHECK(hidapi_input_ring_resize(&r, 3) == 0);
    CHECK(r.capacity == 3 && r.count == 3 && r.head == 0 && r.tail == 0 && r.dropped == 3);
    uint8_t buf[SLOT_SZ];
    for (int expect = 3; expect <= 5; expect++) {
        CHECK(hidapi_input_ring_pop_into(&r, buf, sizeof(buf)) == 1 && buf[0] == (uint8_t)expect);
    }
    hidapi_input_ring_destroy(&r);
    return 0;
}

static int test_resize_shrink_above_count(void) {
    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 8, SLOT_SZ) == 0);
    uint8_t v[1];
    for (int i = 0; i < 3; i++) { v[0] = (uint8_t)i; hidapi_input_ring_push(&r, v, 1); }
    CHECK(hidapi_input_ring_resize(&r, 5) == 0);
    CHECK(r.capacity == 5 && r.count == 3 && r.head == 0 && r.tail == 3 && r.dropped == 0);

    /* Verify FIFO payload order survived the shrink-above-count resize. */
    uint8_t buf[SLOT_SZ];
    for (int expect = 0; expect <= 2; expect++) {
        CHECK(hidapi_input_ring_pop_into(&r, buf, sizeof(buf)) == 1 && buf[0] == (uint8_t)expect);
    }
    hidapi_input_ring_destroy(&r);
    return 0;
}

static int test_resize_same_capacity(void) {
    /* No-op path must not mutate state — not even on a wrapped non-empty ring. */
    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 4, SLOT_SZ) == 0);

    /* Case 1: fresh empty ring. */
    CHECK(hidapi_input_ring_resize(&r, 4) == 0);
    CHECK(r.capacity == 4 && r.count == 0 && r.head == 0 && r.tail == 0);

    /* Case 2: wrapped, full ring. */
    uint8_t v[1];
    for (int i = 0; i < 6; i++) { v[0] = (uint8_t)i; hidapi_input_ring_push(&r, v, 1); }
    CHECK(r.count == 4 && r.head == 2 && r.tail == 2 && r.dropped == 2);
    CHECK(hidapi_input_ring_resize(&r, 4) == 0);
    CHECK(r.capacity == 4 && r.count == 4 && r.head == 2 && r.tail == 2 && r.dropped == 2);

    uint8_t buf[SLOT_SZ];
    for (int expect = 2; expect <= 5; expect++) {
        CHECK(hidapi_input_ring_pop_into(&r, buf, sizeof(buf)) == 1 && buf[0] == (uint8_t)expect);
    }
    hidapi_input_ring_destroy(&r);
    return 0;
}

static int test_resize_empty_ring(void) {
    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 4, SLOT_SZ) == 0);
    CHECK(hidapi_input_ring_resize(&r, 8) == 0);
    CHECK(r.capacity == 8 && r.count == 0 && r.head == 0 && r.tail == 0);
    CHECK(hidapi_input_ring_resize(&r, 2) == 0);
    CHECK(r.capacity == 2 && r.count == 0 && r.head == 0 && r.tail == 0);

    /* Push-then-pop round-trip proves head/tail/lengths stayed coherent
     * through both resizes — a subtle bug could leave metadata valid-looking
     * but the post-resize storage misaligned. */
    uint8_t v[1] = {42};
    CHECK(hidapi_input_ring_push(&r, v, 1) == 0);
    CHECK(r.count == 1);
    uint8_t buf[SLOT_SZ];
    CHECK(hidapi_input_ring_pop_into(&r, buf, sizeof(buf)) == 1 && buf[0] == 42);
    CHECK(r.count == 0);
    hidapi_input_ring_destroy(&r);
    return 0;
}

static int test_resize_invalid_args(void) {
    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 4, SLOT_SZ) == 0);
    CHECK(hidapi_input_ring_resize(&r, 0)   == -1);
    CHECK(hidapi_input_ring_resize(&r, -1)  == -1);
    CHECK(hidapi_input_ring_resize(NULL, 4) == -1);
    CHECK(r.capacity == 4);
    struct hidapi_input_ring uninit = {0};
    CHECK(hidapi_input_ring_resize(&uninit, 4) == -1);
    hidapi_input_ring_destroy(&r);
    return 0;
}

static int test_resize_after_wrap(void) {
    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 4, SLOT_SZ) == 0);
    uint8_t v[1];
    for (int i = 0; i < 7; i++) { v[0] = (uint8_t)i; hidapi_input_ring_push(&r, v, 1); }
    CHECK(r.count == 4 && r.dropped == 3);
    CHECK(hidapi_input_ring_resize(&r, 6) == 0);
    CHECK(r.capacity == 6 && r.count == 4 && r.head == 0 && r.tail == 4);
    uint8_t buf[SLOT_SZ];
    for (int expect = 3; expect <= 6; expect++) {
        CHECK(hidapi_input_ring_pop_into(&r, buf, sizeof(buf)) == 1 && buf[0] == (uint8_t)expect);
    }
    hidapi_input_ring_destroy(&r);
    return 0;
}

static int test_resize_shrink_after_wrap(void) {
    /* Hardest resize index-math path: survivor start = (head + dropped) % capacity. */
    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 4, SLOT_SZ) == 0);
    uint8_t v[1];
    for (int i = 0; i < 7; i++) { v[0] = (uint8_t)i; hidapi_input_ring_push(&r, v, 1); }
    CHECK(r.count == 4 && r.dropped == 3 && r.head != 0);
    CHECK(hidapi_input_ring_resize(&r, 2) == 0);
    CHECK(r.capacity == 2 && r.count == 2 && r.head == 0 && r.tail == 0 && r.dropped == 5);
    uint8_t buf[SLOT_SZ];
    CHECK(hidapi_input_ring_pop_into(&r, buf, sizeof(buf)) == 1 && buf[0] == 5);
    CHECK(hidapi_input_ring_pop_into(&r, buf, sizeof(buf)) == 1 && buf[0] == 6);
    hidapi_input_ring_destroy(&r);
    return 0;
}

static int test_stress(void) {
    /* 10k mixed push/pop/resize schedule. Push-dominated (6/7 push, 1/7 pop)
     * fills the ring around i=90 and keeps it at cap after that, so most
     * subsequent pushes hit drop-oldest. Occasional resize (every 500 iter)
     * exercises resize-under-stream. FIFO invariant checked on every pop. */
    struct hidapi_input_ring r = {0};
    CHECK(hidapi_input_ring_init(&r, 64, SLOT_SZ) == 0);
    uint32_t next_write = 0, next_read = 0;
    uint8_t buf[SLOT_SZ];
    for (int i = 0; i < 10000; i++) {
        if (i > 0 && (i % 500) == 0) {
            int new_cap = ((i / 500) % 2 == 0) ? 32 : 128;
            CHECK(hidapi_input_ring_resize(&r, new_cap) == 0);
            /* Shrink may have evicted oldest — rebase before any subsequent pop. */
            if (r.count > 0 && next_read < next_write - r.count) {
                next_read = next_write - r.count;
            }
        }
        if ((i % 7) == 0 && r.count > 0) {
            int got_bytes = hidapi_input_ring_pop_into(&r, buf, sizeof(buf));
            CHECK(got_bytes == (int)sizeof(uint32_t));
            uint32_t got; memcpy(&got, buf, sizeof(got));
            CHECK(got == next_read);
            next_read++;
        } else {
            CHECK(hidapi_input_ring_push(&r, (uint8_t*)&next_write, sizeof(next_write)) == 0);
            next_write++;
        }
        /* Invariant: next_read >= (next_write - r.count). Rebase if a push
         * drop-oldest or a resize shrink evicted past it. */
        if (r.count > 0 && next_read < next_write - r.count) {
            next_read = next_write - r.count;
        }
    }
    CHECK(r.dropped > 100);
    hidapi_input_ring_destroy(&r);
    return 0;
}

int main(void) {
    int fails = 0;
    struct { const char *name; int (*fn)(void); } tests[] = {
        {"init_destroy",               test_init_destroy},
        {"init_invalid",               test_init_invalid},
        {"destroy_idempotent",         test_destroy_idempotent},
        {"invalid_args",               test_invalid_args},
        {"push_oversized_report",      test_push_oversized_report},
        {"init_overflow_reject",       test_init_overflow_reject},
        {"pop_into_truncation",        test_pop_into_truncation},
        {"pop_into_null_zero",         test_pop_into_null_zero},
        {"push_pop_fifo",              test_push_pop_fifo},
        {"drop_oldest_on_full",        test_drop_oldest_on_full},
        {"drop_oldest_varying_lengths", test_drop_oldest_varying_lengths},
        {"zero_length_report",         test_zero_length_report},
        {"resize_grow",                test_resize_grow},
        {"resize_shrink_below_count",  test_resize_shrink_below_count},
        {"resize_shrink_above_count",  test_resize_shrink_above_count},
        {"resize_same_capacity",       test_resize_same_capacity},
        {"resize_empty_ring",          test_resize_empty_ring},
        {"resize_invalid_args",        test_resize_invalid_args},
        {"resize_after_wrap",          test_resize_after_wrap},
        {"resize_shrink_after_wrap",   test_resize_shrink_after_wrap},
        {"stress",                     test_stress},
    };
    for (size_t i = 0; i < sizeof(tests)/sizeof(tests[0]); i++) {
        int rc = tests[i].fn();
        printf("%s %s\n", rc == 0 ? "PASS" : "FAIL", tests[i].name);
        fails += (rc != 0);
    }
    printf("\n%d/%zu failed\n", fails, sizeof(tests)/sizeof(tests[0]));
    return fails ? 1 : 0;
}
