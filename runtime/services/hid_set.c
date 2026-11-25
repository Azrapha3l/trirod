// hid (shared memory, input state filled in later) and settings (set, set:sys).
#include <string.h>

#include "../hle.h"
#include "../ipc.h"
#include "../settings.h"
#include "hid_shmem.h"

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

// ------------------------------------------------------------------ hid
KSharedMem *g_hid_shmem;
static uint32_t hid_shmem_handle;
// Set by SetNpadJoyHoldType (120); Vertical until then, the service's default (yuzu npad_data.cpp 98) and what
// command 121 always answered before. yuzu's NpadCondition initialiser says Horizontal, but nothing reads that there.
static uint32_t hold_type = HID_HOLD_VERTICAL;
RT_ON_RESET(hid_reset) { g_hid_shmem = NULL; hid_shmem_handle = 0; hold_type = HID_HOLD_VERTICAL; }

// Player 1 (npad id 0) is the one controller the input HLE reports (runtime/hle/input.c), so it's the last active.
static void hid_update_condition(void) {
    if (g_hid_shmem && g_hid_shmem->guest_addr)
        hid_write_npad_condition((uint8_t *)(uintptr_t)g_hid_shmem->guest_addr, 0, hold_type);
}
static void hid_shmem_map(KSharedMem *m) { (void)m; hid_update_condition(); }

static void applet_resource(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0) {  // GetSharedMemoryHandle
        if (!g_hid_shmem) g_hid_shmem = kernel_create_shared_memory(HID_SHMEM_SIZE, hid_shmem_map, &hid_shmem_handle);
        resp->copy_handles[resp->ncopy++] = hid_shmem_handle;
    }
}

SERVICE("hid", hid) {
    (void)s;
    switch (req->cmd) {
    case 0: ipc_return_object(resp, service_new("IAppletResource", applet_resource, NULL)); break;
    case 40: case 106: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;
    case 101: OUT(uint32_t, 0) = 0x1F; resp_size(resp, 4); break;  // GetSupportedNpadStyleSet
    case 120:  // SetNpadJoyHoldType(aruid u64, hold type u64): yuzu cmif_serialization.h 141-150, hid_server.cpp 756
        if (req->data_size < 16 || IN(uint64_t, 8) > HID_HOLD_HORIZONTAL) {
            LOG("[hid] SetNpadJoyHoldType: bad request (%u bytes)\n", req->data_size);
            break;
        }
        if (hold_type != (uint32_t)IN(uint64_t, 8)) LOG("[hid] NpadJoyHoldType %llu\n", (unsigned long long)IN(uint64_t, 8));
        hold_type = (uint32_t)IN(uint64_t, 8);
        hid_update_condition();  // nnSdk 20 reads it back from shared memory (hid_shmem.h)
        break;
    case 121: OUT(int64_t, 0) = hold_type; resp_size(resp, 8); break;  // GetNpadJoyHoldType
    case 129: OUT(int64_t, 0) = 0; resp_size(resp, 8); break;      // GetNpadHandheldActivationMode
    case 201: case 202: case 203: case 206: case 207: case 208: case 209: break;  // vibration
    case 200: memset(resp->data, 0, 0x10); OUT(uint32_t, 0) = 1; resp_size(resp, 0x10); break;
    default: break;  // Activate*/Set*: accept
    }
}

SERVICE("hid:sys", hid_sys) { (void)s; (void)req; resp_size(resp, 0x10); }

// ------------------------------------------------------------------ settings
static const char *const languages[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl",
                                         "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans",
                                         "zh-Hant", "pt-BR"};
#define NLANG 18

static uint64_t lang_code(const char *s) {
    uint64_t v = 0;
    memcpy(&v, s, strlen(s));
    return v;
}

static void language_list(IpcReq *req, IpcResp *resp, int max) {
    IpcBuf *b = out_buf(req, 0);
    int n = 0;
    if (b) {
        uint64_t *out = (uint64_t *)(uintptr_t)b->addr;
        for (; n < max && n < NLANG && (uint64_t)(n + 1) * 8 <= b->size; n++) out[n] = lang_code(languages[n]);
    }
    OUT(int32_t, 0) = n;
    resp_size(resp, 4);
}

SERVICE("set", set) {
    (void)s;
    switch (req->cmd) {
    case 0: OUT(uint64_t, 0) = lang_code(settings_language()); resp_size(resp, 8); break;  // GetLanguageCode
    case 1: language_list(req, resp, 15); break;                                   // GetAvailableLanguageCodes
    case 2: {                                                                      // MakeLanguageCode
        uint32_t i = IN(uint32_t, 0);
        OUT(uint64_t, 0) = i < NLANG ? lang_code(languages[i]) : 0;
        resp_size(resp, 8);
        break;
    }
    case 3: OUT(int32_t, 0) = 15; resp_size(resp, 4); break;
    case 4: OUT(uint32_t, 0) = 1; resp_size(resp, 4); break;                       // GetRegionCode: USA
    case 5: language_list(req, resp, NLANG); break;
    case 6: OUT(int32_t, 0) = NLANG; resp_size(resp, 4); break;
    case 8: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;                        // GetQuestFlag
    default: LOG("[set] cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

SERVICE("set:sys", set_sys) {
    (void)s;
    switch (req->cmd) {
    case 3: case 4: {  // GetFirmwareVersion(2) -> B buffer 0x100
        IpcBuf *b = out_buf(req, 0);
        if (b) {
            uint8_t *v = (uint8_t *)(uintptr_t)b->addr;
            memset(v, 0, 0x100);
            v[0] = 8; v[1] = 1; v[2] = 0; v[4] = 8; v[5] = 1; v[6] = 0;  // 8.1.0
            strcpy((char *)v + 0x68, "8.1.0");
            strcpy((char *)v + 0x80, "8.1.0-1.0");
        }
        break;
    }
    case 23: OUT(uint32_t, 0) = 1; resp_size(resp, 4); break;  // GetColorSetId: dark
    default: LOG("[set:sys] cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}
