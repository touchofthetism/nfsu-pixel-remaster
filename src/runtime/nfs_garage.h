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
 *     on custom hoods, Unlock all decals, Restore brake LOD in race, Show more colors,
 *     and the rim browser sites of All rim sizes (which lists all twelve sizes of a
 *     style whatever the car's tyre; here the tyre stays the car's and the sizes it
 *     lacks are generated, nfs_rims.h).
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
#define NFS_GARAGE_MORE_PAINTS      (1u << 7)  /* body, spoiler, rims and exhaust tips in more paint types */
#define NFS_GARAGE_ALL_RIM_SIZES    (1u << 8)  /* every rim style in every inner size on the car's tyre size (nfs_rims.h) */
#define NFS_GARAGE_ALL              0x1FFu
#define NFS_GARAGE_FEATURE_COUNT    9u

/* Short names (telemetry keys, the Java side's GameOptions.GARAGE_KEYS) and log wording, by bit index. */
static inline const char* nfs_garage_feature_key(unsigned index) {
    static const char* const keys[NFS_GARAGE_FEATURE_COUNT] = {
        "unlock", "specialVinyls", "vinylLayers", "hoodVinyls", "hoodDecals", "allDecals", "raceBrakes", "morePaints",
        "allRimSizes"};
    return index < NFS_GARAGE_FEATURE_COUNT ? keys[index] : "?";
}
static inline const char* nfs_garage_feature_name(unsigned index) {
    static const char* const names[NFS_GARAGE_FEATURE_COUNT] = {
        "everything unlocked", "special vinyls listed", "special vinyls with other layers",
        "vinyls on custom hoods", "decals on custom hoods", "every decal unlocked", "full-detail brakes in races",
        "more paint types", "every rim size"};
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
/* The paint shop's paint types (ARCHIE's ShowMoreColors). 0x4D7B70 (stdcall, the paint target in edi, a list
 * head in esi) empties the list and fills it with the paint types the target is offered, then returns their
 * count: a circular list of 12-byte nodes {next, prev, type} from the game's malloc (0x67121A), freed by the
 * next call (0x671102). The targets are the GaragePaintModScreen's PAINT_MOD_BASE, _RIMS, _BRAKE_CALIPERS,
 * _SPOILER, the vinyl's colours, _ENGINE_ACCENT and _MUFFLER_TIP (0-6); a type is a palette, the paint parts
 * of one kind in the parts database: 0 gloss, 1 metallic, 2 pearl, 3 vinyl, 4 calipers, 5 rims, 6 engine
 * accents, 7 muffler tips. The game offers the base and the spoiler gloss, metallic and pearl (as the career
 * allows), every other target its own palette. ARCHIE's mod rebuilds the list; here a hook on 0x4D7D74,
 * where every path meets before the count, adds what his lists have and the game's lacks, so the count the
 * caller takes (one type: no type picker) includes them. */
#define NFS_GARAGE_HOOK_PAINT_TYPES  0x004D7D74u
#define NFS_GARAGE_GAME_MALLOC       0x0067121Au
#define NFS_PAINT_TARGETS            7u
#define NFS_PAINT_NODE_BYTES         12u

/* ARCHIE's lists by target (the order a type takes in the picker); the targets not listed keep the game's. */
static inline unsigned nfs_garage_paint_types(uint32_t target, const uint8_t** types) {
    static const uint8_t base[] = {0, 1, 2, 4, 5, 7};     /* base paint and spoiler */
    static const uint8_t rims[] = {5, 0, 1, 2, 4};
    static const uint8_t mufflers[] = {7, 0, 1, 2, 4, 5};
    switch (target) {
    case 0: case 3: *types = base; return sizeof base;
    case 1: *types = rims; return sizeof rims;
    case 6: *types = mufflers; return sizeof mufflers;
    default: *types = 0; return 0;
    }
}

/* Guest memory as the hook sees it (the host runtime's accessors; tests use a fake). alloc is the game's own
 * malloc, so the game frees the nodes as its own; 0 when it fails. */
typedef struct nfs_guest_memory {
    uint32_t (*read32)(void* ctx, uint32_t va);
    void (*write32)(void* ctx, uint32_t va, uint32_t value);
    uint32_t (*alloc)(void* ctx, uint32_t bytes);
    void* ctx;
} nfs_guest_memory;

/* Appends each of `types` the circular list at `head` lacks, in order, linked at the tail exactly as
 * 0x4D7B70 links its own nodes. Returns how many were added; stops at an allocation failure. A list longer
 * than 64 nodes, or broken (a null link), is left alone. */
static inline unsigned nfs_garage_append_paint_types(const nfs_guest_memory* m, uint32_t head, const uint8_t* types,
                                                     unsigned count) {
    uint8_t present[8] = {0};
    unsigned nodes = 0;
    for (uint32_t node = m->read32(m->ctx, head); node != head; node = m->read32(m->ctx, node)) {
        if (!node || ++nodes > 64) return 0;
        uint32_t type = m->read32(m->ctx, node + 8);
        if (type < 8) present[type] = 1;
    }
    unsigned added = 0;
    for (unsigned i = 0; i < count; i++) {
        if (types[i] >= 8 || present[types[i]]) continue;
        uint32_t node = m->alloc(m->ctx, NFS_PAINT_NODE_BYTES);
        if (!node) break;
        uint32_t tail = m->read32(m->ctx, head + 4);
        m->write32(m->ctx, node + 8, types[i]);
        m->write32(m->ctx, tail, node);          /* tail->next */
        m->write32(m->ctx, head + 4, node);      /* head->prev */
        m->write32(m->ctx, node + 4, tail);      /* node->prev */
        m->write32(m->ctx, node, head);          /* node->next */
        present[types[i]] = 1;
        added++;
    }
    return added;
}

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
    /* Every rim size (nfs_rims.h; ARCHIE's AllRimSizes nops the same tests): the rim browser 0x50D420 keeps an
     * entry whose INNER_RADIUS is the size the style's tier gives the car ([ebp-20h], 0x50D7E1 for the small
     * brands, 0x50D977 for one brand) or, with [0x6F88A8] clear, the car's present size ([ebp-2Ch]: 0x50D877,
     * 0x50D9EC); not jumping keeps every size. It also drops an entry whose TEXTURE_NAME (the style) is listed
     * already (0x50D8C8, 0x50DA46); not jumping keeps each size of a style. The OUTER_RADIUS tests stay: a
     * car's tyre is its own. */
    {0x0050D7E1u, NFS_GARAGE_ALL_RIM_SIZES, 0},
    {0x0050D877u, NFS_GARAGE_ALL_RIM_SIZES, 0},
    {0x0050D8C8u, NFS_GARAGE_ALL_RIM_SIZES, 0},
    {0x0050D977u, NFS_GARAGE_ALL_RIM_SIZES, 0},
    {0x0050D9ECu, NFS_GARAGE_ALL_RIM_SIZES, 0},
    {0x0050DA46u, NFS_GARAGE_ALL_RIM_SIZES, 0},
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
/* Whether the host serves the generated rim sizes (rim_sizes.h): their parts are then in a second database,
 * which the parts lookup reaches through nfs_garage_parts_lookup_fast. Before boot. */
void nfs_garage_set_rim_sizes_served(int served);
int nfs_garage_rim_sizes_served(void);
/* Fast paths (nfs_fastpath_register, Android): the parts lookup 0x57CE10 across databases (registered while
 * the rim sizes are served), and the rim browser's thumbnail sharing at 0x4F8730 (with every rim size on). */
int nfs_garage_parts_lookup_fast(void);
int nfs_garage_browser_item_fast(void);
/* Every rim size, for the session log: lookups the host answered, records found past the first database,
 * thumbnails shared, browser lists put in size order, and browsers opened on the car's own rim. */
typedef struct nfs_rim_stats {
    uint64_t lookups, later, shared, sorted, opened;
} nfs_rim_stats;
nfs_rim_stats nfs_garage_rim_stats(void);
uint32_t nfs_garage_features(void);
/* Once, after the image is mapped and before the game's entry point runs. */
void nfs_garage_boot(void);
/* The game's malloc for nodes the Garage adds to the game's lists (the host calls 0x67121A); none: no node is
 * added. Before boot. */
void nfs_garage_set_allocator(uint32_t (*alloc)(uint32_t bytes));
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
