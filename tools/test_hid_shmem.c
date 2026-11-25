// hid shared memory NpadCondition (runtime/services/hid_shmem.h) against the reads the v1.1.0 update's nnSdk 20
// client does, transcribed from its sdk module: nn::hid::detail::GetNpadJoyHoldType (sdk 0x5083f0) and
// GetLastActiveNpadController (0x50afb0), on the pointer NpadSharedMemoryAccessor::Attach sets (shmem + 0x3E200).
// No game files: the reads are re-implemented here.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/services/hid_shmem.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define RESULT(module, desc) ((module) | (desc) << 9)

// ldrb w9, [x0, #0xc]; cbz -> 0x58cca; else ldr w8, [x0, #8]
static uint32_t guest_get_hold_type(const uint8_t *shm, uint32_t *out) {
    const uint8_t *c = shm + HID_NPAD_CONDITION;
    if (!c[0xC]) return 0x58cca;
    memcpy(out, c + 8, 4);
    return 0;
}

// ldr x8, [x0]; tst x8, #0xff00000000; b.eq -> 0x5deca; else str w8
static uint32_t guest_last_active(const uint8_t *shm, uint32_t *out) {
    uint64_t v;
    memcpy(&v, shm + HID_NPAD_CONDITION, 8);
    if (!(v & 0xff00000000ull)) return 0x5deca;
    *out = (uint32_t)v;
    return 0;
}

int main(void) {
    // the result constants are HID (202) 710 ResultNpadNotConnected and 751 (yuzu hid_result.h 41)
    CHECK(RESULT(202, 710) == 0x58cca);
    CHECK(RESULT(202, 751) == 0x5deca);
    // offset from the reference's chain: debug_mouse at 0x3DC00 + MouseSharedMemoryFormat 0x400 + 0x200 padding
    CHECK(HID_NPAD_CONDITION == 0x3DC00 + 0x400 + 0x200);
    CHECK(HID_NPAD_CONDITION + 0x10 <= HID_SHMEM_SIZE);

    uint8_t *shm = calloc(1, HID_SHMEM_SIZE);
    uint32_t v = 0xdead;
    // the block as MapSharedMemory hands it over: the abort from the log
    CHECK(guest_get_hold_type(shm, &v) == 0x58cca);
    CHECK(guest_last_active(shm, &v) == 0x5deca);

    hid_write_npad_condition(shm, 0, HID_HOLD_VERTICAL);
    v = 0xdead;
    CHECK(guest_get_hold_type(shm, &v) == 0 && v == HID_HOLD_VERTICAL);
    v = 0xdead;
    CHECK(guest_last_active(shm, &v) == 0 && v == 0);

    hid_write_npad_condition(shm, 0, HID_HOLD_HORIZONTAL);  // after SetNpadJoyHoldType(Horizontal)
    CHECK(guest_get_hold_type(shm, &v) == 0 && v == HID_HOLD_HORIZONTAL);

    // only the 0x10 bytes of the condition are written
    size_t touched = 0;
    for (size_t i = 0; i < HID_SHMEM_SIZE; i++)
        if (shm[i] && (i < HID_NPAD_CONDITION || i >= HID_NPAD_CONDITION + 0x10)) touched++;
    CHECK(touched == 0);
    free(shm);

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("test_hid_shmem: all checks passed\n");
    return 0;
}
