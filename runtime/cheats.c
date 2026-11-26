// Optional single-player cheats (settings.h cheat_*, the menu's Cheats tab). The game's code reads g_cheats at the
// instructions tools/patches.py replaces: the stores of the wallet and of Swordfighter Form energy, and the start of the
// player's status update (life). Nothing here touches guest memory or the save files itself; with a cheat off those
// instructions do exactly what the game's did. (The Might Crystal site, CHEAT_MIGHT, has no setting yet.)
//
// The patch addresses exist for two builds of main only (v1.0.0 a581406bc9858651, v1.1.0 9f221e1afa65ba54); for any
// other the generator applies none, and the menu says so (cheats_available).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheats.h"
#include "cpu.h"
#include "hle.h"
#include "settings.h"

volatile uint32_t g_cheats;
static bool available;

// The builds of main tools/patches.py has cheat sites for (NSO header build id, first 8 bytes).
static const uint8_t known_builds[][8] = {
    {0xa5, 0x81, 0x40, 0x6b, 0xc9, 0x85, 0x86, 0x51},  // v1.0.0
    {0x9f, 0x22, 0x1e, 0x1a, 0xfa, 0x65, 0xba, 0x54},  // v1.1.0
};

void cheats_init(const char *exefs) {
    char path[1100];
    snprintf(path, sizeof path, "%s/main", exefs);
    uint8_t hdr[0x48];
    FILE *f = fopen(path, "rb");
    if (f) {
        if (fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !memcmp(hdr, "NSO0", 4))
            for (size_t i = 0; i < sizeof known_builds / sizeof *known_builds; i++)
                available |= !memcmp(hdr + 0x40, known_builds[i], 8);
        fclose(f);
    }
    if (!available) LOG("[cheats] this build of the game has no cheat patches: the Cheats tab is off\n");
    cheats_sync();
}

bool cheats_available(void) { return available; }

// The settings -> the bits the patched instructions read (called after loading the settings and on every menu change:
// a cheat switched off stops at the next store).
void cheats_sync(void) {
    static int trace = -1;
    if (trace < 0) trace = getenv("EOWRECOMP_CHEAT_TRACE") && atoi(getenv("EOWRECOMP_CHEAT_TRACE")) != 0;
    uint32_t b = 0;
    if (available) {
        if (g_settings.cheat_rupees) b |= CHEAT_RUPEES;
        if (g_settings.cheat_health) b |= CHEAT_HEALTH;
        if (g_settings.cheat_energy) b |= CHEAT_ENERGY;
    }
    if (trace) b |= CHEAT_TRACE;
    g_cheats = b;
}

// EOWRECOMP_CHEAT_TRACE=1 (with EOWRECOMP_VERBOSE=1): every store at a patched site that changes the value, with what
// the game computed and what was stored - how the damage, spending and drain sites were told apart.
void rt_cheat_trace(uint64_t site, uint64_t obj, double old, double computed, double stored) {
    if (old == computed) return;
    LOG_VERBOSE("[cheats] site %llx obj %llx: %g -> %g%s\n", (unsigned long long)site, (unsigned long long)obj, old,
                computed, stored != computed ? " (kept)" : "");
}
