/*
 * fastpath.c - recomp_fastpath: the host's chance to handle a call to a listed guest function
 * (nfs_fastpath.h). A handful of slots, searched in order: the listed functions are called tens of
 * thousands of times a frame, and a short array beats any lookup structure at this size.
 *
 * Shared by the Windows host and the Android runtime.
 */
#include <stdint.h>
#include "nfs_fastpath.h"   /* declares recomp_fastpath as recomp_types.h does; tests/fastpath.cpp includes this file */

typedef struct {
    uint32_t va;
    nfs_fastpath_fn handler;
    uint64_t calls, handled;
} slot_t;

static slot_t s_slots[NFS_FASTPATH_SLOTS];

int nfs_fastpath_register(uint32_t va, nfs_fastpath_fn handler) {
    for (unsigned i = 0; i < NFS_FASTPATH_SLOTS; i++)
        if (s_slots[i].va == va) {
            s_slots[i].handler = handler;
            s_slots[i].calls = s_slots[i].handled = 0;
            if (!handler) s_slots[i].va = 0;
            return 1;
        }
    if (!handler) return 1;
    for (unsigned i = 0; i < NFS_FASTPATH_SLOTS; i++)
        if (!s_slots[i].va) {
            s_slots[i].va = va;
            s_slots[i].handler = handler;
            s_slots[i].calls = s_slots[i].handled = 0;
            return 1;
        }
    return 0;
}

int recomp_fastpath(uint32_t va) {
    for (unsigned i = 0; i < NFS_FASTPATH_SLOTS; i++) {
        slot_t* slot = &s_slots[i];
        if (slot->va != va) continue;
        slot->calls++;
        if (!slot->handler || !slot->handler()) return 0;
        slot->handled++;
        return 1;
    }
    return 0;
}

uint64_t nfs_fastpath_calls(uint32_t va) {
    for (unsigned i = 0; i < NFS_FASTPATH_SLOTS; i++) if (s_slots[i].va == va) return s_slots[i].calls;
    return 0;
}

uint64_t nfs_fastpath_handled(uint32_t va) {
    for (unsigned i = 0; i < NFS_FASTPATH_SLOTS; i++) if (s_slots[i].va == va) return s_slots[i].handled;
    return 0;
}
