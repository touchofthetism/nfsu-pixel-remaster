/*
 * garage.c - customization freedoms for the lifted NFSU1 (nfs_garage.h, nfs_rims.h).
 *
 * recomp_branch decides the conditional jumps config/branches.txt names (the
 * lifter wraps their condition in RECOMP_BRANCH): the lifted outcome stands
 * unless the feature that overrides the jump is on. Hooks set a register
 * result or work on a list the game built, two fast paths answer guest
 * functions, and Unlock everything is the game's own switch, a byte set before
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
#include "nfs_rims.h"

static uint32_t s_features;
static uint64_t s_changes[NFS_GARAGE_FEATURE_COUNT];
static uint64_t s_unknown_sites;
static uint32_t (*s_alloc)(uint32_t bytes);
static int s_rims_served;
static nfs_rim_stats s_rims;
/* The thumbnail decision taken at the rim browser's call to 0x4F8730 (a hook), used by that call's fast path. */
static uint32_t s_share_items, s_share_key;
static int s_share_pending;

static uint32_t guest_read32(void* ctx, uint32_t va) { (void)ctx; return MEM32(va); }
static void guest_write32(void* ctx, uint32_t va, uint32_t value) { (void)ctx; MEM32(va) = value; }
static uint32_t guest_alloc(void* ctx, uint32_t bytes) { (void)ctx; return s_alloc ? s_alloc(bytes) : 0; }
/* The registers are the guest's own wherever this is used; the allocator's guest call saves and restores them. */
static const nfs_guest_memory k_guest = {guest_read32, guest_write32, guest_alloc, 0};

static void count_change(uint32_t feature) {
    for (unsigned i = 0; i < NFS_GARAGE_FEATURE_COUNT; i++)
        if (feature == (1u << i)) { s_changes[i]++; return; }
}

void nfs_garage_set_allocator(uint32_t (*alloc)(uint32_t bytes)) { s_alloc = alloc; }

void nfs_garage_configure(uint32_t features) {
    s_features = features & NFS_GARAGE_ALL;
    for (unsigned i = 0; i < NFS_GARAGE_FEATURE_COUNT; i++) s_changes[i] = 0;
    s_unknown_sites = 0;
    { const nfs_rim_stats none = {0, 0, 0, 0, 0}; s_rims = none; }
    s_share_pending = 0;
}

uint32_t nfs_garage_features(void) { return s_features; }

void nfs_garage_set_rim_sizes_served(int served) { s_rims_served = served ? 1 : 0; }
int nfs_garage_rim_sizes_served(void) { return s_rims_served; }
nfs_rim_stats nfs_garage_rim_stats(void) { return s_rims; }

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

/* 0x57CE10 (stdcall: eax = the list head; car, group, part, previous record, level; ret 14h). A part hash is
 * looked for in every database (a saved rim resolves wherever its record is); a list goes past the database
 * the game stops in only with every rim size on. */
int nfs_garage_parts_lookup_fast(void) {
    const uint32_t esp = g_esp;
    const uint32_t car = MEM32(esp + 4), part = MEM32(esp + 12), after = MEM32(esp + 16);
    const int32_t group = (int32_t)MEM32(esp + 8), level = (int32_t)MEM32(esp + 20);
    const int extend = part != 0 || (s_features & NFS_GARAGE_ALL_RIM_SIZES) != 0;
    g_eax = nfs_parts_lookup(&k_guest, g_eax, car, group, part, after, level, extend, &s_rims.later);
    g_esp = esp + 4 + 0x14;
    s_rims.lookups++;
    return 1;
}

/* 0x4F8730 (stdcall: items, key, 0, 1; ret 10h) as the rim browser calls it for a style's later size: no new
 * item, as when the package has no thumbnail left (eax 0, which 0x50DD40 checks). Any other call runs. */
int nfs_garage_browser_item_fast(void) {
    if (!s_share_pending) return 0;
    const uint32_t esp = g_esp;
    s_share_pending = 0;
    if (MEM32(esp + 4) != s_share_items || MEM32(esp + 8) != s_share_key) return 0;
    g_eax = 0;
    g_esp = esp + 4 + 0x10;
    s_rims.shared++;
    count_change(NFS_GARAGE_ALL_RIM_SIZES);
    return 1;
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
    case NFS_GARAGE_HOOK_PAINT_TYPES: {
        const uint8_t* types;
        unsigned count = nfs_garage_paint_types(g_edi, &types);
        if (!(s_features & NFS_GARAGE_MORE_PAINTS) || !count || !s_alloc) return 1;
        if (nfs_garage_append_paint_types(&k_guest, g_esi, types, count)) count_change(NFS_GARAGE_MORE_PAINTS);
        return 1;
    }
    case NFS_RIM_HOOK_NEW_ITEM:
        /* 0x50DD40 about to make an entry's thumbnail (ebx = the browser's items, ecx = the style): a style's
         * later sizes share its first one's. */
        s_share_pending = 0;
        if ((s_features & NFS_GARAGE_ALL_RIM_SIZES) && nfs_rim_browser_shares_item(&k_guest, g_ebx, g_ecx)) {
            s_share_items = g_ebx; s_share_key = g_ecx; s_share_pending = 1;
        }
        return 1;
    case NFS_RIM_HOOK_LIST_BUILT: {
        /* 0x50D420 built its list and picked the entry to open on (edx = this): sizes in order within each
         * style, and the car's own rim selected when it is listed. */
        if (!(s_features & NFS_GARAGE_ALL_RIM_SIZES)) return 1;
        const uint32_t browser = g_edx;
        if (MEM32(browser) != NFS_RIM_BROWSER_VTABLE) return 1;
        if (nfs_rim_browser_sort(&k_guest, browser)) { s_rims.sorted++; count_change(NFS_GARAGE_ALL_RIM_SIZES); }
        const uint32_t own = nfs_rim_browser_find(&k_guest, browser, MEM32(NFS_RIM_INSTALLED));
        if (own && own != MEM32(browser + NFS_RIM_BROWSER_SELECTED)) {
            MEM32(browser + NFS_RIM_BROWSER_SELECTED) = own;
            s_rims.opened++;
            count_change(NFS_GARAGE_ALL_RIM_SIZES);
        }
        return 1;
    }
    case NFS_RIM_HOOK_INSTALLED: {
        /* 0x50DF60: bl = the selected style is the car's (its TEXTURE_NAME; for the small brands its part).
         * With every size listed, "installed" marks the car's own size only. */
        if (!(s_features & NFS_GARAGE_ALL_RIM_SIZES)) return 1;
        const uint32_t bl = g_edi && g_edi == MEM32(NFS_RIM_INSTALLED) ? 1u : 0u;
        if ((g_ebx & 0xFFu) != bl) { g_ebx = (g_ebx & ~0xFFu) | bl; count_change(NFS_GARAGE_ALL_RIM_SIZES); }
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
