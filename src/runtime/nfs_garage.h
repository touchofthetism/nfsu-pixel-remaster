/* nfs_garage.h - customization freedoms ported to the lifted game (see garage.c).
 *
 * Each feature is one launcher switch (bit). Most PC mods change these by
 * patching a conditional jump into a jmp or nopping it; in the lifted game the
 * jump is a C `if`, so config/branches.txt lists those jumps, the lifter wraps
 * their condition in RECOMP_BRANCH, and recomp_branch (garage.c) decides them
 * from the table below. The three other kinds of patch are a byte of game data
 * (UnlockEverythingThatCanBeLocked) and two register results set by a hook
 * (config/hooks.txt -> nfs_garage_hook).
 *
 * Sources, all for Speed.exe 1.4:
 *   ExOpts Team's NFSU Extra Options (GPL-3.0, ExtraOptionsStuff.h): Unlock all
 *     things, and from Show special vinyls the hidden-vinyl test and its No
 *     Restrictions block. Its other two special-vinyl patches are not ported: one
 *     makes the unique-unlock check (0x5A3570, 27 callers: parts, rims, vinyls)
 *     answer yes for everything, which is Unlock all's job; the other makes the
 *     vinyl unlock table (0x4AA9D0) answer every entry of a kind with the first
 *     entry's state.
 *   ARCHIE's NFSU Extended Customization 1.2 (scripts/NFSU_ExtendedCustomization.asi;
 *     no source published, the patch sites read from its code): Vinyls and Decals
 *     on custom hoods, Unlock all decals, Restore brake LOD in race.
 * Every site was checked against the lifted catalog: a conditional jump that is
 * the first, or only, consumer of the flags its patch relied on.
 *
 * The decisions are header-only so tests/garage.cpp can exercise them.
 */
#ifndef NFS_GARAGE_H
#define NFS_GARAGE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Launcher switches (GameOptions.GARAGE_KEYS on the Java side, same order). */
#define NFS_GARAGE_UNLOCK_ALL       (1u << 0)  /* every car, track, part, rim, vinyl and unique part available */
#define NFS_GARAGE_SPECIAL_VINYLS   (1u << 1)  /* the hidden vinyls listed with the others */
#define NFS_GARAGE_VINYL_LAYERS     (1u << 2)  /* special vinyls combine with the other layers */
#define NFS_GARAGE_HOOD_VINYLS      (1u << 3)  /* vinyls stay on, and go on, a custom hood */
#define NFS_GARAGE_HOOD_DECALS      (1u << 4)  /* hood decals stay on, and go on, a custom hood */
#define NFS_GARAGE_ALL_DECALS       (1u << 5)  /* every decal unlocked */
#define NFS_GARAGE_RACE_BRAKES      (1u << 6)  /* brakes keep their full detail in races */
#define NFS_GARAGE_ALL              0x7Fu
#define NFS_GARAGE_FEATURE_COUNT    7u

/* Short names (telemetry keys, the Java side's GameOptions.GARAGE_KEYS) and log wording, by bit index. */
static inline const char* nfs_garage_feature_key(unsigned index) {
    static const char* const keys[NFS_GARAGE_FEATURE_COUNT] = {
        "unlock", "specialVinyls", "vinylLayers", "hoodVinyls", "hoodDecals", "allDecals", "raceBrakes"};
    return index < NFS_GARAGE_FEATURE_COUNT ? keys[index] : "?";
}
static inline const char* nfs_garage_feature_name(unsigned index) {
    static const char* const names[NFS_GARAGE_FEATURE_COUNT] = {
        "everything unlocked", "special vinyls listed", "special vinyls with other layers",
        "vinyls on custom hoods", "decals on custom hoods", "every decal unlocked", "full-detail brakes in races"};
    return index < NFS_GARAGE_FEATURE_COUNT ? names[index] : "?";
}

/* The game's own unlock-everything switch (a byte in .data; ExOpts _UnlockEverythingThatCanBeLocked).
 * Every lock test reads it (the rim browser 0x50D420, the unique-part check 0x5A3570, the car and track
 * lists...); nothing in the game writes it, and nothing of it reaches the save. */
#define NFS_GARAGE_UNLOCK_FLAG      0x00735EC1u

/* Hooks (config/hooks.txt). */
/* 0x4E2680 answers whether a vinyl is one of the 17 hidden ones (its part name hash against SAMANTHA, EDDIE,
 * JUNKMAN, MELISSA, DDAY and CUSTOM11, 24, 27-35, 37: the story crew's, the band, sponsor, Eastsiderz and Nismo
 * vinyls of category 0Fh) and returns 1 from `mov al, 1` at 0x4E27D6. Extra Options writes `mov al, 0` there; here the hook on the next
 * instruction (pop esi, reached only from there) clears al. */
#define NFS_GARAGE_HOOK_HIDDEN_VINYL 0x004E27D8u
/* The brakes' LOD set in the car renderer (0x56DC30): `xor ecx, ecx / test edx, edx / sete cl` at
 * 0x5702F1-0x5702F7 picks the second set when [esp+0A8h] is 0 (in a race), then `lea ecx, [ecx+ecx*2]`.
 * ARCHIE's mod writes `xor cl, cl` over the sete; the hook on the lea clears cl. */
#define NFS_GARAGE_HOOK_BRAKE_LOD    0x005702F8u

typedef struct nfs_branch_override {
    uint32_t va;       /* the conditional jump */
    uint32_t feature;  /* NFS_GARAGE_* bit that overrides it */
    int take;          /* 1: always jump (a jmp patched over it), 0: never jump (nopped) */
} nfs_branch_override;

/* Sorted by VA; config/branches.txt lists exactly these (tests/garage.cpp checks both). */
static const nfs_branch_override k_nfs_branch_overrides[] = {
    /* Vinyls on custom hoods (ARCHIE): the vinyl menu gate 0x4E4610, option 1, refuses a car whose hood
     * ([0x745B8C]) is not STOCK with a dialog; the jump is the STOCK one. */
    {0x004E465Bu, NFS_GARAGE_HOOD_VINYLS, 1},
    /* Special vinyls with other layers (Extra Options No Restrictions, 0x4E46C9 zeroing the four layer parts
     * [0x745BE8..F4]): the gate refuses a layer whose part is a special vinyl (category [p+12h] == 0Fh)
     * next to others; taking the four "layer empty" jumps reads every layer as empty. */
    {0x004E46E4u, NFS_GARAGE_VINYL_LAYERS, 1},
    {0x004E46F2u, NFS_GARAGE_VINYL_LAYERS, 1},
    {0x004E46FEu, NFS_GARAGE_VINYL_LAYERS, 1},
    {0x004E470Au, NFS_GARAGE_VINYL_LAYERS, 1},
    /* ... and option 4 (a special vinyl) refuses a car with any layer in use (No Restrictions, 0x4E4791
     * jmp 0x4E47CD): the four jumps over `xor bl, bl` keep bl = 1, allowed. */
    {0x004E4791u, NFS_GARAGE_VINYL_LAYERS, 1},
    {0x004E479Bu, NFS_GARAGE_VINYL_LAYERS, 1},
    {0x004E47A5u, NFS_GARAGE_VINYL_LAYERS, 1},
    {0x004E47AFu, NFS_GARAGE_VINYL_LAYERS, 1},
    /* Vinyls on custom hoods (ARCHIE, 0x508AB8: the four vinyl layer parts read as none): installing a
     * custom hood (the parts screen 0x5089B0) warns, then removes vinyl layers of categories 1, 3, 6, 8,
     * 0Ah and 0Dh; the four "no part on this layer" jumps leave every category 0. */
    {0x00508AD1u, NFS_GARAGE_HOOD_VINYLS, 1},
    {0x00508ADDu, NFS_GARAGE_HOOD_VINYLS, 1},
    {0x00508AE9u, NFS_GARAGE_HOOD_VINYLS, 1},
    {0x00508AF5u, NFS_GARAGE_HOOD_VINYLS, 1},
    /* Decals on custom hoods (ARCHIE): installing a custom hood asks to remove the hood decals (0x5088C0
     * true) and, confirmed (message F0AADEBBh, slot 8), removes them (call 0x508910). */
    {0x00508B86u, NFS_GARAGE_HOOD_DECALS, 1},   /* je over the dialog, as if no decal were on */
    {0x00508F52u, NFS_GARAGE_HOOD_DECALS, 1},   /* jne over the removal call */
    /* ... the decal menu 0x5101F0 refuses a non-STOCK hood (ARCHIE: cmp eax, eax at 0x5104FC). */
    {0x005104FEu, NFS_GARAGE_HOOD_DECALS, 1},
    /* All decals (ARCHIE UnlockAllDecals, mov eax, 1 at 0x512223): the decal lock test 0x511FF0 calls the
     * unlock check unless [0x7677AC] is set; the jump is the set one. */
    {0x0051222Au, NFS_GARAGE_ALL_DECALS, 1},
    /* Decals on custom hoods (ARCHIE: the attribute key E80A3B62h, EXCLUDEDECAL, zeroed in three compares):
     * the ride update 0x57EA80 drops the hood decal slots a hood part excludes; never finding the attribute
     * keeps them. */
    {0x0057EAF6u, NFS_GARAGE_HOOD_DECALS, 0},
    {0x0057EB16u, NFS_GARAGE_HOOD_DECALS, 0},
    {0x0057EBA6u, NFS_GARAGE_HOOD_DECALS, 0},
};
#define NFS_BRANCH_OVERRIDE_COUNT (sizeof k_nfs_branch_overrides / sizeof k_nfs_branch_overrides[0])

/* The table index of `va`, or -1. */
static inline int nfs_branch_override_index(uint32_t va) {
    int lo = 0, hi = (int)NFS_BRANCH_OVERRIDE_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t at = k_nfs_branch_overrides[mid].va;
        if (at == va) return mid;
        if (at < va) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

/* Whether the jump at `va` is taken, given its lifted outcome and the enabled features. */
static inline int nfs_branch_decision(uint32_t features, uint32_t va, int taken) {
    int i = nfs_branch_override_index(va);
    if (i < 0 || !(features & k_nfs_branch_overrides[i].feature)) return taken ? 1 : 0;
    return k_nfs_branch_overrides[i].take;
}

/* The al the hidden-vinyl test returns: 0 (not hidden) with special vinyls on. */
static inline uint32_t nfs_garage_hidden_vinyl_eax(uint32_t features, uint32_t eax) {
    return (features & NFS_GARAGE_SPECIAL_VINYLS) ? (eax & ~0xFFu) : eax;
}
/* The ecx the brake LOD choice leaves: 0 (the first, full-detail set) with race brakes on. */
static inline uint32_t nfs_garage_brake_lod_ecx(uint32_t features, uint32_t ecx) {
    return (features & NFS_GARAGE_RACE_BRAKES) ? (ecx & ~0xFFu) : ecx;
}

/* Before the guest boots (the launcher's switches). */
void nfs_garage_configure(uint32_t features);
uint32_t nfs_garage_features(void);
/* Once, after the image is mapped and before the game's entry point runs. */
void nfs_garage_boot(void);
/* recomp_hook dispatch: 1 when `va` was one of these features' sites. */
int nfs_garage_hook(uint32_t va);
/* Decisions an enabled feature changed so far, by feature bit index (diagnostics), and the RECOMP_BRANCH
 * sites reached that the table does not know (should stay 0). */
uint64_t nfs_garage_changes(unsigned feature_index);
uint64_t nfs_branch_unknown_sites(void);

#ifdef __cplusplus
}
#endif
#endif
