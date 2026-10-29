/* nfs_rims.h - every rim size on every car (Garage switch allRimSizes, garage.c): the game-side half.
 *
 * The host serves a wheel pack (android/app/src/main/cpp/rim_sizes.h) whose resident section is a second
 * parts database holding the rim sizes Speed.exe 1.4 lacks; the game links it after GLOBALB's. Two things in
 * the game then need to reach it, and the rim browser has to offer every size:
 *
 * 1. The parts lookup 0x57CE10 (stdcall, eax = the database list head 0x73AD40; car hash, group, part hash,
 *    previous record, level), 79 callers: the save loader 0x4AC5A0 resolving a saved part by hash, the rim
 *    browser and every other list stepping through parts. Each database's 48-byte records are sorted by car
 *    hash; a car's are found by binary search (0x53E400) and read while the car hash holds. The game was
 *    written for one database: it gives up at the end of that run, or when the search finds no record, in
 *    the first database that has the car, and a search continuing after a record never leaves that record's
 *    database. nfs_parts_lookup is the same search, record for record, which with `extend` goes on into the
 *    later databases instead: always for a part hash (a saved rim must resolve wherever its record is), and
 *    for lists only with the switch on (off, every list reads exactly the parts the game's own lookup does).
 *
 * 2. The rim browser 0x50D420 (MU_GarageRimsBrowser.fng) lists a brand's styles, one entry each: the size
 *    the style's tier gives the car (inner = outer - 9 + tier, tier 4 as tier 3: [0x6F88A8], set, makes a
 *    style's size follow its tier) on the car's tyre (OUTER_RADIUS == CarTypeInfo +0xDC), and it drops a
 *    second entry of a style (by TEXTURE_NAME). With the switch on, branches.txt lets every inner size of the
 *    car's tyre size through and keeps the style's later sizes. Each entry takes a thumbnail object of the
 *    package (20 "thumb" objects; ENKEI has 10 styles, 60 sizes), so a style's later sizes make none
 *    (nfs_rim_browser_shares_item, before 0x4F8730): moving onto them highlights the style's thumbnail
 *    (0x4F8910 selects the first item of a key) while the info panel shows the size (0x50DEE0) and the
 *    preview car wears it. After the list is built the entries are put in style order, smallest size first
 *    (nfs_rim_browser_sort), and the browser opens on the car's own rim when it is listed.
 *
 * Header-only: tests/rim_sizes.cpp drives it with a fake guest memory.
 */
#ifndef NFS_RIMS_H
#define NFS_RIMS_H
#include <stdint.h>
#include "nfs_garage.h"   /* nfs_guest_memory */
#ifdef __cplusplus
extern "C" {
#endif

#define NFS_PARTS_LOOKUP          0x0057CE10u   /* the lookup (fast path) */
#define NFS_PARTS_LIST_HEAD       0x0073AD40u   /* the databases' circular list: {next, prev}; a node is the header payload */
#define NFS_PARTS_RECORD_BYTES    48u
#define NFS_PARTS_MAX_DATABASES   64u           /* a longer list is taken as broken */
#define NFS_ATTR_INNER_RADIUS     0xEB0101E2u   /* BinHash("INNER_RADIUS") */
#define NFS_ATTR_TEXTURE_NAME     0x10C98090u

/* The rim browser (0x50D420 builds it; this = the screen object). */
#define NFS_RIM_BROWSER_VTABLE    0x006C01BCu
#define NFS_RIM_BROWSER_ENTRIES   0x44u   /* circular list head {next, prev}; this+0x40 the selected entry */
#define NFS_RIM_BROWSER_SELECTED  0x40u
#define NFS_RIM_BROWSER_ITEMS     0x4Cu   /* the horizontal browser (vtable 0x6C1870): items list head at +4 */
#define NFS_RIM_ENTRY_KEY         0x08u   /* TEXTURE_NAME: a style */
#define NFS_RIM_ENTRY_RECORD      0x34u
#define NFS_RIM_INSTALLED         0x00745BB0u   /* the car's rim when the browser opened (restored on leaving) */
#define NFS_RIM_BROWSER_MAX       512u
/* Hooks and the thumbnail fast path. */
#define NFS_RIM_HOOK_LIST_BUILT   0x0050DB35u   /* 0x50D420 after it picked the entry to open on: edx = this */
#define NFS_RIM_HOOK_NEW_ITEM     0x0050DDBBu   /* 0x50DD40 at its call 0x4F8730(this+0x4C, key, 0, 1): ebx = this+0x4C, ecx = key */
#define NFS_RIM_HOOK_INSTALLED    0x0050E046u   /* 0x50DF60 after `sete bl` (selected style installed?): edi = the selected record */
#define NFS_BROWSER_ADD_ITEM      0x004F8730u   /* the horizontal browser's add (stdcall, ret 10h): returns the item's object */

/* 0x53E400 on the records of one database: the first record whose car hash (+4) is `car`, or 0. */
static inline uint32_t nfs_parts_bsearch(const nfs_guest_memory* m, uint32_t records, uint32_t count, uint32_t car) {
    if (!records || (int32_t)count <= 0) return 0;
    int32_t lo = 0, hi = (int32_t)count - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        uint32_t at = records + (uint32_t)mid * NFS_PARTS_RECORD_BYTES;
        uint32_t key = m->read32(m->ctx, at + 4);
        if (car < key) hi = mid - 1;
        else if (car > key) lo = mid + 1;
        else {
            while (mid > 0 && m->read32(m->ctx, at - NFS_PARTS_RECORD_BYTES + 4) == car) { at -= NFS_PARTS_RECORD_BYTES; mid--; }
            return at;
        }
    }
    return 0;
}

/* 0x57CE10: the first record after `after` (0: from the start) with the car hash (0: any), group (byte +10h,
 * signed; -1: any), part hash (+8; 0: any) and level (byte +11h; -1: any). `extend` 0 is the game's search;
 * 1 continues past the database the game stops in. `later`, when not null, counts a record found in a
 * database after the first. */
static inline uint32_t nfs_parts_lookup(const nfs_guest_memory* m, uint32_t head, uint32_t car, int32_t group, uint32_t part,
                                        uint32_t after, int32_t level, int extend, uint64_t* later) {
    unsigned nodes = 0;
    const uint32_t first = m->read32(m->ctx, head);
    for (uint32_t node = first; node != head; node = m->read32(m->ctx, node)) {
        if (!node || ++nodes > NFS_PARTS_MAX_DATABASES) return 0;
        const uint32_t records = m->read32(m->ctx, node + 0xC), count = m->read32(m->ctx, node + 0x10);
        const uint32_t end = records + count * NFS_PARTS_RECORD_BYTES;
        uint32_t p;
        if (after) {
            if (after < records || after >= end) continue;   /* not this database's record */
            p = after + NFS_PARTS_RECORD_BYTES;
            if (extend) after = 0;                           /* the later databases from their start */
        } else if (car) {
            p = nfs_parts_bsearch(m, records, count, car);
            if (!p) { if (extend) continue; return 0; }
        } else {
            p = records;
        }
        for (; p < end; p += NFS_PARTS_RECORD_BYTES) {
            if (car && m->read32(m->ctx, p + 4) != car) { if (extend) break; return 0; }
            const uint32_t bytes = m->read32(m->ctx, p + 0x10);
            if (group != -1 && (int32_t)(int8_t)(bytes & 0xFFu) != group) continue;
            if (part && m->read32(m->ctx, p + 8) != part) continue;
            if (level != -1 && (int32_t)((bytes >> 8) & 0xFFu) != level) continue;
            if (later && node != first) ++*later;
            return p;
        }
    }
    return 0;
}

/* A record's integer attribute (0 when absent): the attributes pointer (+14h, fixed up by the loader) and
 * their count (word +1Ch); pairs {key, value}. */
static inline uint32_t nfs_part_attribute(const nfs_guest_memory* m, uint32_t record, uint32_t key) {
    const uint32_t list = m->read32(m->ctx, record + 0x14), count = m->read32(m->ctx, record + 0x1C) & 0xFFFFu;
    if (!list) return 0;
    for (uint32_t i = 0; i < count && i < 256u; ++i)
        if (m->read32(m->ctx, list + 8 * i) == key) return m->read32(m->ctx, list + 8 * i + 4);
    return 0;
}

/* Before 0x50DD40 makes an entry's thumbnail: whether the horizontal browser `items` (this+4Ch: its items'
 * list head at +4, an item's key at +8) already has one for `key` - the style's first size has it. */
static inline int nfs_rim_browser_shares_item(const nfs_guest_memory* m, uint32_t items, uint32_t key) {
    const uint32_t head = items + 4;
    unsigned seen = 0;
    for (uint32_t item = m->read32(m->ctx, head); item != head; item = m->read32(m->ctx, item)) {
        if (!item || ++seen > NFS_RIM_BROWSER_MAX) return 0;
        if (m->read32(m->ctx, item + 8) == key) return 1;
    }
    return 0;
}

/* After 0x50D420 built its entries (this+44h, a circular list of {next, prev, key...}): relinks them by style
 * (in the order each style first appears, which is the thumbnails' order) and, within a style, by inner size
 * (stable). Returns 1 when the order changed. A list longer than NFS_RIM_BROWSER_MAX, or broken, is left alone. */
static inline int nfs_rim_browser_sort(const nfs_guest_memory* m, uint32_t browser) {
    const uint32_t head = browser + NFS_RIM_BROWSER_ENTRIES;
    uint32_t entry[NFS_RIM_BROWSER_MAX], rank[NFS_RIM_BROWSER_MAX], size[NFS_RIM_BROWSER_MAX], keys[NFS_RIM_BROWSER_MAX];
    unsigned n = 0, styles = 0;
    for (uint32_t e = m->read32(m->ctx, head); e != head; e = m->read32(m->ctx, e)) {
        if (!e || n == NFS_RIM_BROWSER_MAX) return 0;
        const uint32_t key = m->read32(m->ctx, e + NFS_RIM_ENTRY_KEY), record = m->read32(m->ctx, e + NFS_RIM_ENTRY_RECORD);
        unsigned r = 0;
        while (r < styles && keys[r] != key) ++r;
        if (r == styles) keys[styles++] = key;
        entry[n] = e; rank[n] = r; size[n] = record ? nfs_part_attribute(m, record, NFS_ATTR_INNER_RADIUS) : 0; ++n;
    }
    if (n < 2) return 0;
    /* insertion sort (stable; a brand lists at most a few dozen entries) */
    int changed = 0;
    for (unsigned i = 1; i < n; ++i) {
        const uint32_t e = entry[i], r = rank[i], s = size[i];
        unsigned j = i;
        while (j > 0 && (rank[j - 1] > r || (rank[j - 1] == r && size[j - 1] > s))) {
            entry[j] = entry[j - 1]; rank[j] = rank[j - 1]; size[j] = size[j - 1]; --j; changed = 1;
        }
        entry[j] = e; rank[j] = r; size[j] = s;
    }
    if (!changed) return 0;
    uint32_t prev = head;
    for (unsigned i = 0; i < n; ++i) {
        m->write32(m->ctx, prev, entry[i]);        /* prev->next */
        m->write32(m->ctx, entry[i] + 4, prev);    /* entry->prev */
        prev = entry[i];
    }
    m->write32(m->ctx, prev, head);
    m->write32(m->ctx, head + 4, prev);
    return 1;
}

/* The entry whose record is `record` (the car's rim), or 0. */
static inline uint32_t nfs_rim_browser_find(const nfs_guest_memory* m, uint32_t browser, uint32_t record) {
    const uint32_t head = browser + NFS_RIM_BROWSER_ENTRIES;
    unsigned seen = 0;
    if (!record) return 0;
    for (uint32_t e = m->read32(m->ctx, head); e != head; e = m->read32(m->ctx, e)) {
        if (!e || ++seen > NFS_RIM_BROWSER_MAX) return 0;
        if (m->read32(m->ctx, e + NFS_RIM_ENTRY_RECORD) == record) return e;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif
#endif
