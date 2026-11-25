// hid shared memory (nn::hid::detail::SharedMemoryFormat, 0x40000 bytes): the parts the runtime fills.
// Layout from yuzu's hid_core/resources/shared_memory_format.h (offset asserts, lines 230-248) and
// hid_core/resources/npad/npad_types.h (NpadCondition, lines 168-175).
#pragma once
#include <stdint.h>
#include <string.h>

#define HID_SHMEM_SIZE 0x40000
#define HID_NPAD_CONDITION 0x3E200  // nn::hid::NpadCondition, 0x10 bytes

// NpadJoyHoldType (npad_types.h 25-29)
#define HID_HOLD_VERTICAL 0u
#define HID_HOLD_HORIZONTAL 1u

// NpadCondition {u32 last active npad id; u32 valid; u32 hold type; u32 valid}. The nnSdk 20 client (the v1.1.0
// update's sdk) reads it instead of asking the service: nn::hid::detail::GetNpadJoyHoldType (sdk 0x5083f0) returns
// the word at +8 if the byte at +0xC is set, else HID 710 (ResultNpadNotConnected), which nn::hid::GetNpadJoyHoldType
// turns into an abort; GetLastActiveNpadController (0x50afb0) returns the word at +0 if the byte at +4 is set, else
// HID 751. NpadSharedMemoryAccessor::Attach (0x5080a0) points the accessor at shared memory + 0x3E200. A zeroed
// block (what MapSharedMemory hands over) fails both. nnSdk 16 (v1.0.0) asks with commands 121 and doesn't read it.
static inline void hid_write_npad_condition(uint8_t *shmem, uint32_t last_active_npad, uint32_t hold_type) {
    uint32_t c[4] = {last_active_npad, 1, hold_type, 1};
    memcpy(shmem + HID_NPAD_CONDITION, c, sizeof c);
}
