/*
 * freeroam.c - free roam's game-side half (nfs_freeroam.h): the track records and names it adds, no wrong way and
 * resets where the car was driving on its tracks. The decisions are nfs_freeroam.h's (tests/free_roam.cpp); this
 * file reads and writes the game's memory for them.
 *
 * Shared by the Windows host and the Android runtime (the Windows host configures no tracks: every entry point
 * then leaves the game alone).
 */
#include <stdint.h>
#include <string.h>
#ifdef NFS_TESTHOST
#include <stdio.h>
#endif
#include "recomp_types.h"
#include "nfs_freeroam.h"

#define RESET_MIN_AGE   (3u * NFS_CRUMB_INTERVAL)   /* a reset goes to where the car was 3 s before, not into the crash */

static nfs_freeroam_track s_tracks[NFS_FREEROAM_MAX_TRACKS];
static unsigned s_count;
static nfs_freeroam_host s_host;
static nfs_freeroam_stats s_stats;
static nfs_crumb_trail s_trail;
static uint32_t s_scratch;             /* 16 guest bytes: the position a reset passes to 0x427C50 */
static uint32_t s_names[NFS_FREEROAM_MAX_TRACKS];
static uint32_t s_race_record;         /* the race record the trail belongs to */

static uint32_t guest_read32(void* ctx, uint32_t va) { (void)ctx; return MEM32(va); }
static void guest_write32(void* ctx, uint32_t va, uint32_t value) { (void)ctx; MEM32(va) = value; }
static uint32_t guest_alloc(void* ctx, uint32_t bytes) { (void)ctx; return s_host.alloc ? s_host.alloc(bytes) : 0; }
static const nfs_guest_memory k_guest = {guest_read32, guest_write32, guest_alloc, 0};

void nfs_freeroam_configure(const nfs_freeroam_track* tracks, unsigned count, const nfs_freeroam_host* host) {
    unsigned i;
    s_count = tracks ? (count > NFS_FREEROAM_MAX_TRACKS ? NFS_FREEROAM_MAX_TRACKS : count) : 0;
    for (i = 0; i < s_count; ++i) s_tracks[i] = tracks[i];
    if (host) s_host = *host; else memset(&s_host, 0, sizeof s_host);
    memset(&s_stats, 0, sizeof s_stats);
    memset(&s_trail, 0, sizeof s_trail);
    s_scratch = 0;
    for (i = 0; i < NFS_FREEROAM_MAX_TRACKS; ++i) s_names[i] = 0;
    s_race_record = 0;
}

unsigned nfs_freeroam_track_count(void) { return s_count; }
nfs_freeroam_stats nfs_freeroam_get_stats(void) { return s_stats; }

int nfs_freeroam_active(void) {
    uint32_t record;
    unsigned i, number;
    if (!s_count) return 0;
    record = MEM32(NFS_TRACKINFO_CURRENT);
    if (!record) return 0;
    number = MEM16(record + 0x6C);
    for (i = 0; i < s_count; ++i) if (s_tracks[i].number == number) return 1;
    return 0;
}

int nfs_freeroam_hook(uint32_t va) {
    uint32_t table, count;
    switch (va) {
    case NFS_TRACKINFO_LOADED:   /* 0x442040 pointed [0x736168] at the chunk's records */
        if (!s_count) return 1;
        table = MEM32(NFS_TRACKINFO_TABLE); count = MEM32(NFS_TRACKINFO_COUNT);
        s_stats.tracks_added += nfs_freeroam_add_records(&k_guest, s_tracks, s_count, &table, &count);
        MEM32(NFS_TRACKINFO_TABLE) = table; MEM32(NFS_TRACKINFO_COUNT) = count;
        return 1;
    case NFS_LANGUAGE_LOADED:    /* 0x59FCE0 pointed [0x734434] at the language's {hash, string} pairs */
        if (!s_count) return 1;
        table = MEM32(NFS_LANGUAGE_TABLE); count = MEM32(NFS_LANGUAGE_COUNT);
        s_stats.names_added += nfs_freeroam_add_names(&k_guest, s_tracks, s_count, s_names, &table, &count);
        MEM32(NFS_LANGUAGE_TABLE) = table; MEM32(NFS_LANGUAGE_COUNT) = count;
        return 1;
    case NFS_LIST_ICON_HASHED:   /* 0x4F42F0 returned the hash of TRACK_<n>_ICON */
        if (s_count) g_eax = nfs_freeroam_entry_icon(s_tracks, s_count, g_eax);
        return 1;
    case NFS_LIST_REVERSE_TEST: {   /* SELECT LOCATION's reversed loop: eax = the record's race types (ebx) */
        const uint32_t types = nfs_freeroam_reverse_types(s_tracks, s_count, MEM16(g_ebx + 0x6C), g_eax);
        if (types != g_eax) s_stats.reverse_entries_left_out++;
        g_eax = types;
        return 1;
    }
    default:
        return 0;
    }
}

/* 0x430E00 (stdcall player, previous; ret 8): on a free roam track the car is never going the wrong way. */
int nfs_freeroam_wrong_way_fast(void) {
    const uint32_t esp = g_esp;
    if (!nfs_freeroam_active()) return 0;
    s_stats.wrong_way_calls++;
    if (MEM8(esp + 8)) s_stats.wrong_way_cleared++;   /* it was flagged the frame before */
    g_eax = 0;
    g_esp = esp + 4 + 8;
    return 1;
}

static float read_float(uint32_t va) { float f; uint32_t v = MEM32(va); memcpy(&f, &v, 4); return f; }
static uint32_t float_bits(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }

/* 0x430730 (eax = the player), every frame: a breadcrumb a second while the car drives. Always declines, so the
 * game's update runs as it is. */
int nfs_freeroam_update_fast(void) {
    uint32_t player, car, record, now;
    nfs_car_state state;
    nfs_crumb crumb;
    if (!nfs_freeroam_active()) return 0;
    record = MEM32(NFS_TRACKINFO_CURRENT);
    now = MEM32(NFS_GAME_CLOCK);
    if (nfs_crumb_trail_for(&s_trail, &s_race_record, record, now)) s_stats.races++;   /* another race, or a restart */
    s_stats.updates++;
    player = g_eax;
    car = player ? MEM32(player + NFS_PLAYER_CAR) : 0;
    if (!car) return 0;
    state.x = read_float(car + NFS_CAR_POSITION);
    state.y = read_float(car + NFS_CAR_POSITION + 4);
    state.z = read_float(car + NFS_CAR_POSITION + 8);
    state.vx = read_float(car + NFS_CAR_VELOCITY);
    state.vy = read_float(car + NFS_CAR_VELOCITY + 4);
    state.vz = read_float(car + NFS_CAR_VELOCITY + 8);
    state.fx = read_float(car + NFS_CAR_FORWARD);
    state.fy = read_float(car + NFS_CAR_FORWARD + 4);
    state.up_z = read_float(car + NFS_CAR_UP + 8);
    state.reset_start = MEM32(car + NFS_CAR_RESET_START);
    state.reset_length = read_float(car + NFS_CAR_RESET_LENGTH);
#ifdef NFS_TESTHOST
    {   /* the test host's record of the car, a line a second (tools/testhost/README.md) */
        static uint32_t logged;
        if (now - logged >= NFS_CRUMB_INTERVAL) {
            logged = now;
            fprintf(stderr, "[freeroam] t=%u pos %.1f %.1f %.1f vel %.2f %.2f %.2f fwd %.3f %.3f up %.3f %.3f %.3f heading %u minimap %u crumbs %u\n",
                    now, state.x, state.y, state.z, state.vx, state.vy, state.vz, state.fx, state.fy, read_float(car + NFS_CAR_UP),
                    read_float(car + NFS_CAR_UP + 4), state.up_z, (unsigned)MEM16(car + 0x3B4), (unsigned)MEM8(0x0078A3F6u), s_trail.count);
        }
    }
#endif
    if (!nfs_freeroam_crumb(&s_trail, &state, now, &crumb)) return 0;
    nfs_crumb_add(&s_trail, &crumb);
    s_stats.crumbs++;
    return 0;
}

/* 0x4314A0 (eax = the player; ret): on a free roam track, the car goes back to where it was driving three
 * seconds before, facing the way it went, placed by the game's own code as 0x4314A0's off-route branch does. */
int nfs_freeroam_reset_fast(void) {
    const uint32_t esp = g_esp, saved_edi = g_edi, saved_ecx = g_ecx, saved_esi = g_esi;
    uint32_t player, car, physics, table, heading = 0, result = 0, args[3];
    const nfs_crumb* crumb;
    if (!nfs_freeroam_active()) return 0;
    s_stats.resets++;
    player = g_eax;
    car = player ? MEM32(player + NFS_PLAYER_CAR) : 0;
    crumb = nfs_crumb_pick(&s_trail, MEM32(NFS_GAME_CLOCK), RESET_MIN_AGE);
    if (!car || !crumb || !s_host.call || !s_host.alloc) { s_stats.resets_left++; return 0; }
    if (!s_scratch) s_scratch = s_host.alloc(16);
    physics = MEM32(car + NFS_CAR_PHYSICS);
    table = physics ? MEM32(physics) : 0;
    if (!s_scratch || !table) { s_stats.resets_left++; return 0; }
    /* what 0x4314A0 does first, to the player */
    if (MEM32(player + NFS_PLAYER_STATE) == 6 || MEM32(player + NFS_PLAYER_STATE) == 7) {
        g_ecx = player;
        args[0] = 1;
        s_host.call(NFS_PLAYER_LEAVE_STATE, args, 1, 0, &result);
    }
    if (MEM8(NFS_RESET_FLAG_A) || MEM32(NFS_RESET_FLAG_B)) {
        g_ecx = player + NFS_PLAYER_PART;
        s_host.call(NFS_PLAYER_PART_RESET, args, 0, 0, &result);
    }
    g_ecx = saved_ecx;
    /* the heading the game derives from a forward vector (as 0x4314A0 does from the car's) */
    args[0] = float_bits(crumb->fx); args[1] = float_bits(crumb->fy);
    if (!s_host.call(NFS_HEADING_OF, args, 2, 8, &heading)) { s_stats.resets_left++; return 0; }
    MEM32(s_scratch) = float_bits(crumb->x);
    MEM32(s_scratch + 4) = float_bits(crumb->y);
    MEM32(s_scratch + 8) = float_bits(crumb->z);
    MEM32(s_scratch + 12) = 0;
    /* from here as 0x4314A0's off-route branch: edi the car throughout, esi its route tracker (car + 0x800), which
     * 0x443A10 works on without being passed it */
    g_edi = car;
    g_esi = car + NFS_CAR_TRACKER;
    args[0] = s_scratch; args[1] = 0; args[2] = heading & 0xFFFFu;
    if (!s_host.call(NFS_CAR_PLACE, args, 3, 0, &result)) { g_edi = saved_edi; g_esi = saved_esi; s_stats.resets_left++; return 0; }
    args[0] = NFS_ROUTE; args[1] = car + NFS_CAR_POSITION;
    s_host.call(NFS_ROUTE_TRACK, args, 2, 0, &result);
    g_ecx = physics;
    args[0] = 1;
    s_host.call(MEM32(table + 0x2C), args, 1, 0, &result);
    g_ecx = physics;
    s_host.call(MEM32(table + 0x30), args, 0, 0, &result);
    MEM32(car + NFS_CAR_RESET_START) = MEM32(NFS_GAME_CLOCK);
    MEM32(car + NFS_CAR_RESET_LENGTH) = MEM32(NFS_RESET_FLASH);
    g_edi = saved_edi;
    g_ecx = saved_ecx;
    g_esi = saved_esi;
    g_esp = esp + 4;
    s_stats.resets_placed++;
#ifdef NFS_TESTHOST
    fprintf(stderr, "[freeroam] reset to %.1f %.1f %.1f heading %u (crumb t=%u, %u in the trail)\n", crumb->x, crumb->y, crumb->z,
            heading & 0xFFFFu, crumb->tick, s_trail.count);
#endif
    return 1;
}
