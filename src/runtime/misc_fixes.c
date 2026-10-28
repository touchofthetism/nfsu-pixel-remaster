/*
 * misc_fixes.c - small PC-community fixes for the lifted NFSU1 (nfs_misc_fixes.h).
 *
 * From ThirteenAG's NFSUnderground.WidescreenFix (WidescreenFixesPack, MIT),
 * Misc.ixx, beyond its FrameRate block (framerate.c):
 *   - 44.1 kHz audio: ported (nfs_misc_fixes_boot).
 *   - Heap (bMalloc 0x565B60 / bFree 0x565CE0 under one lock): deliberately
 *     not ported. On a multi-core PC two game threads can be inside the
 *     allocation-list update at the same instant (bMalloc links the new block
 *     at 0x565C39-0x565C58, bFree unlinks it at 0x565D26-0x565D3E, both outside
 *     the pool's critical section, and the allocation counter is bumped at
 *     0x565CB2-0x565CC5). Here every guest thread runs under the one guest
 *     machine lock and is only switched at a yield point: a loop back edge,
 *     an import call or a blocking wait. None lies inside those read-modify-
 *     write sequences (straight-line code, no calls, no backward branches),
 *     so each executes as a unit; the pool free lists themselves are guarded
 *     by real Win32 critical sections (0x63F190/0x63F1A0), which the runtime
 *     implements. The 0xAAAAAAAA corruption the mod prevents cannot arise.
 *   - Skip intro, language select, icons, controls-menu names, best-lap
 *     counter: front-end conveniences for the PC, not ported.
 * From AeroWidescreen's NFSUHDContentSupport (NFSU part): HD Cursor Support,
 * ported (nfs_misc_hook). Its HD Font Support is not needed: the fonts live in
 * Languages/LanguageTextures.bin, which no HD pack rewrites.
 *
 * Shared by the Windows host and the Android runtime.
 */
#include <stdint.h>
#include "recomp_types.h"
#include "nfs_misc_fixes.h"

void nfs_misc_fixes_boot(void) {
    MEM32(NFS_MISC_SOUND_RATE_OVERRIDE) = NFS_MISC_FULL_SOUND_RATE;
}

int nfs_misc_hook(uint32_t va) {
    if (va == NFS_MISC_HOOK_CURSOR_SIZE) {
        /* edi: the arrow's texture record, about to be read at +0x44. */
        uint32_t size = MEM32(g_edi + 0x44);
        uint32_t wanted = nfs_misc_cursor_size(MEM32(g_edi + 0x24), size);
        if (wanted != size) MEM32(g_edi + 0x44) = wanted;
        return 1;
    }
    return 0;
}
