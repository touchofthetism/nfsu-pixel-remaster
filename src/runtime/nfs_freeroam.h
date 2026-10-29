/* nfs_freeroam.h - free roam (build 19): the game-side half (freeroam.c). The host side, what the game is served
 * for tracks 1000, 1300 and 1400, is android/app/src/main/cpp/free_roam.h.
 *
 * The Free Roam Mod Beta 2.0 adds its tracks to GLOBALB's TrackInfo table and patches Speed.exe against the
 * wrong-way warning. Here nothing on disk changes (an HD GLOBALB stays as it is):
 *
 * 1. Track records. The TrackInfo loader 0x442040 points [0x736168] at the chunk's records (0xFC each) and
 *    puts their count in [0x73616C]; every lookup scans them by track number (+0x6C), the Free Run list by the
 *    race types a record offers (+0x74). A hook after it (0x44206E) appends one record per free roam track: the
 *    stock 1001 record under the track's number, name, directory and region, offered in Free Run only (bit 4),
 *    its map placed where the host's map lies. The table moves to memory from the game's malloc; the chunk
 *    stays loaded, nothing frees either.
 *
 *    SELECT LOCATION's list (0x4BA100) offers every record its race type passes twice, as it is and then
 *    reversed, except a drift race's (bit 8 of +0x74) reversed. A hook on that test (0x4BA207) gives a free roam
 *    track the drift races' answer: one direction. An entry's icon is the texture TRACK_<n>_ICON, the game's
 *    placeholder when there is none: a hook where its hash is made (0x4BA2FA) gives a free roam track a stock
 *    race's icon. The picture behind the list (the district's map, the route over it) is the host's: textures
 *    added to the dynamic texture pack it serves (free_roam.h).
 *
 * 2. Names. A track's name in the menus is the language string TRACK_NAME_<n> (0x4AD6D0 hashes it); missing,
 *    the menu keeps its .fng default ("Currently Selected Track"). The language loader 0x59FCE0 points
 *    [0x734434] at {hash, string} pairs sorted by hash, [0x73442C] their count, and the lookup 0x59FB80 binary
 *    searches them. A hook after the loader (0x59FD2E) merges in the free roam names, in a copied table.
 *
 * 3. Wrong way. The player's update 0x430730 sets [player+0x4AC] from 0x430E00 (stdcall player, previous; ret 8),
 *    which compares the car's heading with the race route's. A free roam track keeps its base race's route, so
 *    driving anywhere against it would warn and count down: on free roam tracks the fast path on 0x430E00
 *    answers "not wrong way".
 *
 * 4. Reset. 0x4314A0 (eax = the player) puts the car back on the race route (the nearest route point, or its route
 *    tracker's): in free roam always the base race's route, wherever the car was. Every reset goes through it (the
 *    reset key, flipped or stuck cars). On free roam tracks the fast path places the car where it was driving instead:
 *    breadcrumbs, one a second, taken while the car moves on its wheels (the player's update, observed through a fast
 *    path on 0x430730 that always lets the game's code run); a reset uses the newest one at least three seconds old
 *    (so the car does not land in the crash it left), facing the way it was going. It does what 0x4314A0 does, with
 *    the crumb for the route point: first what every reset does to the player (a player in state 6 or 7 leaves it,
 *    0x42DDC0; the object at +0x9E0 is reset when either flag says so, 0x43EF00), then the game's own placement as its
 *    off-route branch makes it (0x427C50, the route tracker 0x443A10, the physics object's reset methods, the reset
 *    flash).
 *
 * Header-only decisions (record building, name merging, breadcrumb choice) so tests/free_roam.cpp can run them.
 */
#ifndef NFS_FREEROAM_H
#define NFS_FREEROAM_H
#include <stdint.h>
#include <string.h>
#include "nfs_garage.h"   /* nfs_guest_memory */
#ifdef __cplusplus
extern "C" {
#endif

#define NFS_FREEROAM_MAX_TRACKS   3u
#define NFS_TRACK_RECORD_BYTES    0xFCu
/* +0x74: the race types a track is offered in, a bit each; 0x4AB470 tests the bit of the race type a list is
 * for ([0x777CC8]): 0 circuit 0x01, 1 sprint 0x04, 2 0x40, 3 Free Run 0x10, 4 0x20, 5 drag 0x02, 6 drift 0x08.
 * Free Run's list (race type 3, read from the game in its track list) shows the circuits, which have 0x71. */
#define NFS_TRACK_MODE_FREE_RUN   0x10u
#define NFS_TRACK_MODE_DRIFT      0x08u     /* SELECT LOCATION offers no reversed entry for a record with it */

#define NFS_TRACKINFO_LOADED      0x0044206Eu   /* hook: 0x442040 set the table and count */
#define NFS_TRACKINFO_TABLE       0x00736168u
#define NFS_TRACKINFO_COUNT       0x0073616Cu
#define NFS_TRACKINFO_CURRENT     0x00736174u   /* the race's record */
#define NFS_LANGUAGE_LOADED       0x0059FD2Eu   /* hook: 0x59FCE0 set the string table */
#define NFS_LIST_ICON_HASHED      0x004BA2FAu   /* hook: eax = BinHash("TRACK_<n>_ICON") of a track list entry */
#define NFS_LIST_REVERSE_TEST     0x004BA207u   /* hook: ebx = the record, eax = its race types, before "test al, 8" */
#define NFS_LANGUAGE_COUNT        0x0073442Cu
#define NFS_LANGUAGE_TABLE        0x00734434u
#define NFS_WRONG_WAY_TEST        0x00430E00u   /* fast path (stdcall player, previous; ret 8) */
#define NFS_PLAYER_UPDATE         0x00430730u   /* fast path that observes (eax = player) and always declines */
#define NFS_PLAYER_RESET          0x004314A0u   /* fast path (eax = player; ret) */
#define NFS_PLAYER_STATE          0x60u         /* player +0x60: 6 and 7 are states a reset first leaves */
#define NFS_PLAYER_LEAVE_STATE    0x0042DDC0u   /* thiscall (player; 1); ret 4: what 0x4314A0 calls in states 6/7 */
#define NFS_PLAYER_PART           0x9E0u        /* player +0x9E0: an object 0x4314A0 resets when either flag is set */
#define NFS_PLAYER_PART_RESET     0x0043EF00u   /* thiscall (player + 0x9E0); ret */
#define NFS_RESET_FLAG_A          0x0078A345u   /* byte */
#define NFS_RESET_FLAG_B          0x007361A4u   /* dword */
#define NFS_CAR_PLACE             0x00427C50u   /* edi = car; stdcall (position*, speed, heading); ret 0Ch */
#define NFS_ROUTE_TRACK           0x00443A10u   /* esi = the car's route tracker; stdcall (route, position*); ret 8 */
#define NFS_ROUTE                 0x007372B0u
#define NFS_HEADING_OF            0x00564DB0u   /* cdecl (x, y) -> 16-bit heading */
#define NFS_GAME_CLOCK            0x0073AD34u   /* ticks */
#define NFS_RESET_FLASH           0x006B7B5Cu   /* the reset flash's length the game sets at a reset */

/* The player and car (Speed.exe 1.4): player +4 the car; car +0x2C its physics object, +0x60 position (x, y,
 * z), +0x70 velocity, +0x90/+0xA0/+0xB0 its orientation (forward, side, up: read in the test host, level and at
 * rest up is (0, 0, 1), and the forward axis turns with the car), +0x800 its route tracker, +0x884/+0x888 the
 * reset flash (start tick, length). The direction at +0x818 the game's own reset faces the car in is the race's
 * (in free roam it stays the start's, whichever way the car goes), so a breadcrumb takes the forward axis. */
#define NFS_PLAYER_CAR            0x04u
#define NFS_CAR_PHYSICS           0x2Cu
#define NFS_CAR_POSITION          0x60u
#define NFS_CAR_VELOCITY          0x70u
#define NFS_CAR_TRACKER           0x800u    /* the car's route tracker, esi of 0x443A10 */
#define NFS_CAR_FORWARD           0x90u     /* the orientation's forward axis (x, y, z) */
#define NFS_CAR_ORIENTATION       0x90u     /* rows of 4 floats: +0x90 forward, +0xA0 the side, +0xB0 up */
#define NFS_CAR_UP                0xB0u     /* the up axis (x, y, z): (0, 0, 1) on level ground */
#define NFS_CAR_RESET_START       0x884u
#define NFS_CAR_RESET_LENGTH      0x888u

typedef struct {
    uint16_t number;                 /* 1000 */
    uint16_t base;                   /* the stock race whose record it copies (1001) */
    char region[8];                  /* "L1RA" */
    char region_directory[32];       /* "Location1\RegionA" */
    char name[32];                   /* "Olympic City" */
    float map_x, map_y, map_width;   /* the made map's placement; width 0 keeps the base record's */
    uint16_t icon;                   /* the stock race whose icon its track list entry shows (0: the placeholder) */
} nfs_freeroam_track;

/* BinHash of "TRACK_<n>_ICON". */
static inline uint32_t nfs_freeroam_icon_hash(unsigned number);

/* A free roam record from its base: name (+0x00, +0xDC), directory (+0x20), region (+0x40, +0x48), number
 * (+0x6C, +0x6E), Free Run only (+0x74), the map's placement (+0x8C..+0x98). The rest is the base's, as in
 * Beta 2.0's records. */
static inline void nfs_freeroam_record(uint8_t* out, const uint8_t* base, const nfs_freeroam_track* t) {
    char directory[32];
    unsigned n = t->number, i, digits = 0;
    char number[8];
    memcpy(out, base, NFS_TRACK_RECORD_BYTES);
    memset(out + 0x00, 0, 0x20); memcpy(out + 0x00, t->name, strnlen(t->name, 0x1F));
    memset(directory, 0, sizeof directory);
    memcpy(directory, "Location1\\Track_", 16);
    do { number[digits++] = (char)('0' + n % 10); n /= 10; } while (n && digits < sizeof number);
    for (i = 0; i < digits && 16 + i < sizeof directory - 1; ++i) directory[16 + i] = number[digits - 1 - i];
    memset(out + 0x20, 0, 0x20); memcpy(out + 0x20, directory, strnlen(directory, 0x1F));
    memset(out + 0x40, 0, 0x08); memcpy(out + 0x40, t->region, strnlen(t->region, 0x07));
    memset(out + 0x48, 0, 0x20); memcpy(out + 0x48, t->region_directory, strnlen(t->region_directory, 0x1F));
    out[0x6C] = (uint8_t)(t->number & 0xFF); out[0x6D] = (uint8_t)(t->number >> 8);
    out[0x6E] = out[0x6C]; out[0x6F] = out[0x6D];
    out[0x74] = (uint8_t)NFS_TRACK_MODE_FREE_RUN; out[0x75] = out[0x76] = out[0x77] = 0;
    if (t->map_width > 0) {
        memcpy(out + 0x8C, &t->map_x, 4); memcpy(out + 0x90, &t->map_y, 4); memcpy(out + 0x94, &t->map_width, 4);
        out[0x98] = out[0x99] = 0;   /* no rotation */
    }
    memset(out + 0xDC, 0, 0x20); memcpy(out + 0xDC, t->name, strnlen(t->name, 0x1F));
}

/* BinHash, as the game's 0x4F42F0 hashes a formatted key. */
static inline uint32_t nfs_bin_hash(const char* text) {
    uint32_t hash = 0xFFFFFFFFu;
    for (; *text; ++text) hash = hash * 33u + (uint8_t)*text;
    return hash;
}

static inline uint32_t nfs_freeroam_icon_hash(unsigned number) {
    char text[24], digits[8];
    unsigned n = number, count = 0, i;
    do { digits[count++] = (char)('0' + n % 10); n /= 10; } while (n && count < sizeof digits);
    memcpy(text, "TRACK_", 6);
    for (i = 0; i < count; ++i) text[6 + i] = digits[count - 1 - i];
    memcpy(text + 6 + count, "_ICON", 6);
    return nfs_bin_hash(text);
}

/* The race types SELECT LOCATION's reversed loop tests for a record (0x4BA207): a free roam track's with the drift
 * bit, so it is offered in one direction only, as a drift race. */
static inline uint32_t nfs_freeroam_reverse_types(const nfs_freeroam_track* tracks, unsigned count, unsigned number,
                                                   uint32_t types) {
    unsigned i;
    for (i = 0; i < count; ++i)
        if (tracks[i].number == number) return types | NFS_TRACK_MODE_DRIFT;
    return types;
}

/* The icon hash a track list entry shows: a free roam track's stock icon for its own (missing) one. */
static inline uint32_t nfs_freeroam_entry_icon(const nfs_freeroam_track* tracks, unsigned count, uint32_t hash) {
    unsigned i;
    for (i = 0; i < count; ++i)
        if (tracks[i].icon && hash == nfs_freeroam_icon_hash(tracks[i].number)) return nfs_freeroam_icon_hash(tracks[i].icon);
    return hash;
}

/* The language keys a free roam track is named by: TRACK_NAME_<n> and REVERSE_TRACK_NAME_<n>. */
static inline void nfs_freeroam_name_keys(unsigned number, uint32_t keys[2]) {
    char text[40], digits[8];
    unsigned n = number, count = 0, i;
    do { digits[count++] = (char)('0' + n % 10); n /= 10; } while (n && count < sizeof digits);
    memcpy(text, "TRACK_NAME_", 11);
    for (i = 0; i < count; ++i) text[11 + i] = digits[count - 1 - i];
    text[11 + count] = 0;
    keys[0] = nfs_bin_hash(text);
    memcpy(text, "REVERSE_TRACK_NAME_", 19);
    for (i = 0; i < count; ++i) text[19 + i] = digits[count - 1 - i];
    text[19 + count] = 0;
    keys[1] = nfs_bin_hash(text);
}

/* Merges `extra` {hash, string} pairs into a hash-sorted table of `count` pairs: writes the result (at most
 * count + extra_count pairs) to `out`, keeping the table's own entry for a hash it already has. Returns the
 * merged count. Pairs are 8 bytes {hash, guest pointer}. */
static inline uint32_t nfs_merge_strings(const nfs_guest_memory* m, uint32_t table, uint32_t count,
                                         const uint32_t* extra, uint32_t extra_count, uint32_t out) {
    uint32_t i = 0, j = 0, n = 0;
    /* extra is sorted here (insertion sort: a handful of pairs) */
    uint32_t sorted[2 * 2 * NFS_FREEROAM_MAX_TRACKS];
    uint32_t k, e = extra_count > 2 * NFS_FREEROAM_MAX_TRACKS ? 2 * NFS_FREEROAM_MAX_TRACKS : extra_count;
    for (k = 0; k < e; ++k) {
        uint32_t h = extra[2 * k], v = extra[2 * k + 1], p = k;
        while (p > 0 && sorted[2 * (p - 1)] > h) { sorted[2 * p] = sorted[2 * (p - 1)]; sorted[2 * p + 1] = sorted[2 * (p - 1) + 1]; --p; }
        sorted[2 * p] = h; sorted[2 * p + 1] = v;
    }
    while (i < count || j < e) {
        uint32_t th = i < count ? m->read32(m->ctx, table + 8 * i) : 0xFFFFFFFFu;
        if (j < e && (i >= count || sorted[2 * j] < th)) {
            m->write32(m->ctx, out + 8 * n, sorted[2 * j]); m->write32(m->ctx, out + 8 * n + 4, sorted[2 * j + 1]);
            ++j; ++n;
        } else {
            if (j < e && sorted[2 * j] == th) ++j;   /* the game's own string wins */
            m->write32(m->ctx, out + 8 * n, th); m->write32(m->ctx, out + 8 * n + 4, m->read32(m->ctx, table + 8 * i + 4));
            ++i; ++n;
        }
    }
    return n;
}

/* Reads a 0xFC-byte record at `va` into `out` (records are whole words). */
static inline void nfs_freeroam_read_record(const nfs_guest_memory* m, uint32_t va, uint8_t* out) {
    uint32_t i, v;
    for (i = 0; i < NFS_TRACK_RECORD_BYTES; i += 4) { v = m->read32(m->ctx, va + i); memcpy(out + i, &v, 4); }
}
static inline void nfs_freeroam_write_record(const nfs_guest_memory* m, uint32_t va, const uint8_t* record) {
    uint32_t i, v;
    for (i = 0; i < NFS_TRACK_RECORD_BYTES; i += 4) { memcpy(&v, record + i, 4); m->write32(m->ctx, va + i, v); }
}

/* The TrackInfo table with the free roam records, as the hook after 0x442040 makes it: a track whose number the
 * table already has (a GLOBALB from Beta 2.0) gets its record rewritten in place; the others are appended to a
 * copy of the table from `m->alloc`. Every track's base record must be in the table, else it is not the game's
 * table and nothing changes. *table and *records become the table to use; returns the records written. */
static inline unsigned nfs_freeroam_add_records(const nfs_guest_memory* m, const nfs_freeroam_track* tracks,
                                                unsigned count, uint32_t* table, uint32_t* records) {
    uint8_t base[NFS_TRACK_RECORD_BYTES], record[NFS_TRACK_RECORD_BYTES];
    uint32_t bases[NFS_FREEROAM_MAX_TRACKS], existing[NFS_FREEROAM_MAX_TRACKS];
    uint32_t i, j, missing = 0, out, n;
    unsigned written = 0;
    if (!*table || !*records || *records > 4096 || !count || count > NFS_FREEROAM_MAX_TRACKS) return 0;
    for (i = 0; i < count; ++i) {
        bases[i] = existing[i] = 0;
        for (j = 0; j < *records; ++j) {
            const uint32_t r = *table + j * NFS_TRACK_RECORD_BYTES;
            const unsigned number = m->read32(m->ctx, r + 0x6C) & 0xFFFFu;
            if (number == tracks[i].base && !bases[i]) bases[i] = r;
            if (number == tracks[i].number && !existing[i]) existing[i] = r;
        }
        if (!bases[i]) return 0;
        if (!existing[i]) ++missing;
    }
    for (i = 0; i < count; ++i) {   /* in place first: the bases stay where they are */
        if (!existing[i]) continue;
        nfs_freeroam_read_record(m, bases[i], base);
        nfs_freeroam_record(record, base, &tracks[i]);
        nfs_freeroam_write_record(m, existing[i], record);
        ++written;
    }
    if (!missing) return written;
    out = m->alloc ? m->alloc(m->ctx, (*records + missing) * NFS_TRACK_RECORD_BYTES) : 0;
    if (!out) return written;
    for (j = 0; j < *records; ++j) {
        nfs_freeroam_read_record(m, *table + j * NFS_TRACK_RECORD_BYTES, record);
        nfs_freeroam_write_record(m, out + j * NFS_TRACK_RECORD_BYTES, record);
    }
    n = *records;
    for (i = 0; i < count; ++i) {
        if (existing[i]) continue;
        nfs_freeroam_read_record(m, bases[i], base);
        nfs_freeroam_record(record, base, &tracks[i]);
        nfs_freeroam_write_record(m, out + n * NFS_TRACK_RECORD_BYTES, record);
        ++n;
        ++written;
    }
    *table = out;
    *records = n;
    return written;
}

/* The language table with the free roam names, as the hook after 0x59FCE0 makes it: each track's name (made once
 * in guest memory, kept in names[]) under TRACK_NAME_<n> and REVERSE_TRACK_NAME_<n>, merged into a copy of the
 * hash-sorted {hash, string} table; a hash the language already has keeps its own string. *table and *pairs
 * become the table to use; returns the names added. */
static inline unsigned nfs_freeroam_add_names(const nfs_guest_memory* m, const nfs_freeroam_track* tracks, unsigned count,
                                              uint32_t names[], uint32_t* table, uint32_t* pairs) {
    uint32_t extra[4 * NFS_FREEROAM_MAX_TRACKS], keys[2], out, n, i, k, extras = 0;
    if (!*table || !*pairs || *pairs > (1u << 20) || !count || count > NFS_FREEROAM_MAX_TRACKS || !m->alloc) return 0;
    for (i = 0; i < count; ++i) {
        if (!names[i]) {
            const size_t length = strnlen(tracks[i].name, sizeof tracks[i].name - 1);
            const uint32_t words = (uint32_t)(length / 4 + 1);   /* the terminator included */
            uint32_t at = m->alloc(m->ctx, words * 4);
            if (!at) return 0;
            for (k = 0; k < words; ++k) {
                uint8_t word[4] = {0, 0, 0, 0};
                uint32_t v, c;
                for (c = 0; c < 4; ++c) if (k * 4 + c < length) word[c] = (uint8_t)tracks[i].name[k * 4 + c];
                memcpy(&v, word, 4);
                m->write32(m->ctx, at + k * 4, v);
            }
            names[i] = at;
        }
        nfs_freeroam_name_keys(tracks[i].number, keys);
        extra[2 * extras] = keys[0]; extra[2 * extras + 1] = names[i]; ++extras;
        extra[2 * extras] = keys[1]; extra[2 * extras + 1] = names[i]; ++extras;
    }
    out = m->alloc(m->ctx, (*pairs + extras) * 8);
    if (!out) return 0;
    n = nfs_merge_strings(m, *table, *pairs, extra, extras, out);
    i = n - *pairs;
    *table = out;
    *pairs = n;
    return i;
}

/* Breadcrumbs: where the car was driving, one a second. */
#define NFS_CRUMBS 64u
typedef struct {
    float x, y, z, fx, fy;   /* position, forward */
    uint32_t tick;           /* the game's clock ([0x73AD34]) */
} nfs_crumb;
typedef struct {
    nfs_crumb crumbs[NFS_CRUMBS];
    uint32_t next, count;
    uint32_t last_tick;
} nfs_crumb_trail;

/* The crumb a reset at `now` goes to: the newest at least `min_age` ticks old, else the oldest kept; NULL with
 * none. A crumb from after `now` (the clock started again) is never taken. */
static inline const nfs_crumb* nfs_crumb_pick(const nfs_crumb_trail* trail, uint32_t now, uint32_t min_age) {
    uint32_t i;
    const nfs_crumb* oldest = 0;
    for (i = 0; i < trail->count; ++i) {
        const nfs_crumb* c = &trail->crumbs[(trail->next + NFS_CRUMBS - 1 - i) % NFS_CRUMBS];   /* newest first */
        if ((int32_t)(now - c->tick) < 0) continue;
        if (now - c->tick >= min_age) return c;
        oldest = c;
    }
    return oldest;
}
/* A trail for the race at `record` and `now`: the same race's goes on; another race, or the clock having started
 * again (the race restarted: same record, new clock), begins a new one. Returns 1 when it began a new one. */
static inline int nfs_crumb_trail_for(nfs_crumb_trail* trail, uint32_t* trail_record, uint32_t record, uint32_t now) {
    if (record == *trail_record && !(trail->count && (int32_t)(now - trail->last_tick) < 0)) return 0;
    memset(trail, 0, sizeof *trail);
    *trail_record = record;
    return 1;
}
static inline void nfs_crumb_add(nfs_crumb_trail* trail, const nfs_crumb* crumb) {
    trail->crumbs[trail->next] = *crumb;
    trail->next = (trail->next + 1) % NFS_CRUMBS;
    if (trail->count < NFS_CRUMBS) ++trail->count;
    trail->last_tick = crumb->tick;
}

/* What the player's update shows of the car, for the breadcrumb decision. */
typedef struct {
    float x, y, z;                 /* +0x60 position */
    float vx, vy, vz;              /* +0x70 velocity */
    float fx, fy;                  /* +0x90 the forward axis, its horizontal part */
    float up_z;                    /* how upright: the up axis's height, 1 on its wheels on level ground */
    uint32_t reset_start;          /* +0x884: the tick the last reset's flash began (0 or 0x7FFFFFFF: none) */
    float reset_length;            /* +0x888: its length in seconds */
} nfs_car_state;

#define NFS_CRUMB_INTERVAL    4000u   /* ticks: one crumb a second ([0x73AD34] counts at 4 kHz) */
#define NFS_CRUMB_MIN_SPEED   3.33f   /* m/s, 12 km/h: a car this fast is driving, not stuck or crashed */
#define NFS_CRUMB_MIN_UP      0.7f    /* within about 45 degrees of upright */

/* Whether the car at `now` leaves a breadcrumb, and which: a second after the trail's last one, the car moving
 * on its wheels, outside a reset's flash (where the game put it, not where it drove), every value finite. */
static inline int nfs_freeroam_crumb(const nfs_crumb_trail* trail, const nfs_car_state* car, uint32_t now, nfs_crumb* out) {
    const float speed2 = car->vx * car->vx + car->vy * car->vy + car->vz * car->vz;
    const float level = car->fx * car->fx + car->fy * car->fy;
    if (trail->count && now - trail->last_tick < NFS_CRUMB_INTERVAL) return 0;
    if (car->reset_start && car->reset_start != 0x7FFFFFFFu &&
        (float)(now - car->reset_start) / (float)NFS_CRUMB_INTERVAL < car->reset_length) return 0;
    if (!(speed2 >= NFS_CRUMB_MIN_SPEED * NFS_CRUMB_MIN_SPEED) || !(speed2 < 1e6f)) return 0;
    if (!(car->up_z >= NFS_CRUMB_MIN_UP) || !(level >= 0.25f) || !(level <= 1.5f)) return 0;
    if (!(car->x == car->x) || !(car->y == car->y) || !(car->z == car->z)) return 0;   /* NaN */
    if (car->x * 0.0f != 0.0f || car->y * 0.0f != 0.0f || car->z * 0.0f != 0.0f) return 0;   /* infinite */
    out->x = car->x; out->y = car->y; out->z = car->z;
    out->fx = car->fx; out->fy = car->fy;
    out->tick = now;
    return 1;
}

/* Telemetry. */
typedef struct {
    uint32_t tracks_added, names_added;
    uint64_t wrong_way_calls, wrong_way_cleared;
    uint64_t updates, crumbs;
    uint64_t resets, resets_placed, resets_left;   /* left: the game's own reset ran (no crumb, a race track) */
    uint32_t races;                                /* free roam sessions seen (race records of a free roam track) */
    uint32_t reverse_entries_left_out;             /* SELECT LOCATION entries left out (a free roam track reversed) */
} nfs_freeroam_stats;

/* The host's services: the game's malloc and a guest call (registers as set; 0 on failure). */
typedef struct {
    uint32_t (*alloc)(uint32_t bytes);
    int (*call)(uint32_t va, const uint32_t* args, unsigned count, uint32_t cleanup, uint32_t* result);
} nfs_freeroam_host;

void nfs_freeroam_configure(const nfs_freeroam_track* tracks, unsigned count, const nfs_freeroam_host* host);
unsigned nfs_freeroam_track_count(void);
int nfs_freeroam_hook(uint32_t va);             /* recomp_hook dispatch */
int nfs_freeroam_wrong_way_fast(void);          /* 0x430E00 */
int nfs_freeroam_update_fast(void);             /* 0x430730: observes, declines */
int nfs_freeroam_reset_fast(void);              /* 0x4314A0 */
int nfs_freeroam_active(void);                  /* the current race is on a free roam track */
nfs_freeroam_stats nfs_freeroam_get_stats(void);

#ifdef __cplusplus
}
#endif
#endif
