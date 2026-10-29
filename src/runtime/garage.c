/*
 * garage.c - customization freedoms for the lifted NFSU1 (nfs_garage.h).
 *
 * recomp_branch decides the conditional jumps config/branches.txt names (the
 * lifter wraps their condition in RECOMP_BRANCH): the lifted outcome stands
 * unless the feature that overrides the jump is on. Two hooks set a register
 * result, and Unlock everything is the game's own switch, a byte set before
 * the game runs.
 *
 * Counters record each decision an enabled feature changed, so the session log
 * shows that a feature was reached, not only that it was on.
 *
 * Shared by the Windows host and the Android runtime.
 */
#include <stdint.h>
#include "recomp_types.h"
#include "nfs_garage.h"

static uint32_t s_features;
static uint64_t s_changes[NFS_GARAGE_FEATURE_COUNT];
static uint64_t s_unknown_sites;

void nfs_garage_configure(uint32_t features) {
    s_features = features & NFS_GARAGE_ALL;
    for (unsigned i = 0; i < NFS_GARAGE_FEATURE_COUNT; i++) s_changes[i] = 0;
    s_unknown_sites = 0;
}

uint32_t nfs_garage_features(void) { return s_features; }

static void count_change(uint32_t feature) {
    for (unsigned i = 0; i < NFS_GARAGE_FEATURE_COUNT; i++)
        if (feature == (1u << i)) { s_changes[i]++; return; }
}

void nfs_garage_boot(void) {
    if (s_features & NFS_GARAGE_UNLOCK_ALL) MEM8(NFS_GARAGE_UNLOCK_FLAG) = 1;
}

int recomp_branch(uint32_t va, int taken) {
    int i = nfs_branch_override_index(va);
    if (i < 0) { s_unknown_sites++; return taken ? 1 : 0; }
    const nfs_branch_override* site = &k_nfs_branch_overrides[i];
    if (!(s_features & site->feature)) return taken ? 1 : 0;
    if ((taken ? 1 : 0) != site->take) count_change(site->feature);
    return site->take;
}

int nfs_garage_hook(uint32_t va) {
    switch (va) {
    case NFS_GARAGE_HOOK_HIDDEN_VINYL: {
        uint32_t eax = nfs_garage_hidden_vinyl_eax(s_features, g_eax);
        if (eax != g_eax) { g_eax = eax; count_change(NFS_GARAGE_SPECIAL_VINYLS); }
        return 1;
    }
    case NFS_GARAGE_HOOK_BRAKE_LOD: {
        uint32_t ecx = nfs_garage_brake_lod_ecx(s_features, g_ecx);
        if (ecx != g_ecx) { g_ecx = ecx; count_change(NFS_GARAGE_RACE_BRAKES); }
        return 1;
    }
    default:
        return 0;
    }
}

uint64_t nfs_garage_changes(unsigned feature_index) {
    return feature_index < NFS_GARAGE_FEATURE_COUNT ? s_changes[feature_index] : 0;
}

uint64_t nfs_branch_unknown_sites(void) { return s_unknown_sites; }
