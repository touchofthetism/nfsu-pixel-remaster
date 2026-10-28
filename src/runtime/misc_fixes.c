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
 *   - BlackMagazineFix (BlackMagazineFix.ixx): ported (nfs_misc_hook). The
 *     magazine covers' background was cleared to black by the 3D pass whenever
 *     Light Trails or Motion Blur was on, which the launcher's "Game graphics:
 *     Maximum" turns on for every new profile.
 * From AeroWidescreen's NFSUHDContentSupport (NFSU part, GPL-3.0): HD Cursor
 * Support (build 9) and HD Font Support (build 14, for the HD fonts of Plajer's
 * HD User Interface in pack v12), ported (nfs_misc_hook). Its Global Neon and
 * Global Shadow options are off by default and need textures no pack adds: not
 * ported.
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
    switch (va) {
    case NFS_MISC_HOOK_CURSOR_SIZE: {
        /* edi: the arrow's texture record, about to be read at +0x44. */
        uint32_t size = MEM32(g_edi + 0x44);
        uint32_t wanted = nfs_misc_cursor_size(MEM32(g_edi + 0x24), size);
        if (wanted != size) MEM32(g_edi + 0x44) = wanted;
        return 1;
    }
    case NFS_MISC_HOOK_FONT_SIZE_TEXT: {
        /* ecx: the font's texture record; its size was just loaded into edx, [esp+0Ch] and eax. */
        uint32_t width, height;
        if (nfs_misc_font_stock_size(MEM32(g_ecx + 0x24), &width, &height)) {
            g_edx = width;
            MEM32(g_esp + 0x0C) = width;
            g_eax = height;
        }
        return 1;
    }
    case NFS_MISC_HOOK_FONT_SIZE_GLYPH: {
        /* [edi+8]: the font's texture record; its size was just loaded into [esp+4] and edx. */
        uint32_t width, height;
        if (nfs_misc_font_stock_size(MEM32(MEM32(g_edi + 8) + 0x24), &width, &height)) {
            MEM32(g_esp + 4) = width;
            g_edx = height;
        }
        return 1;
    }
    case NFS_MISC_HOOK_WORLD_VIEWPORT: {
        /* [esp]: the clear flag 0x40ABD0 is about to take for the 3D view. */
        uint32_t clear = MEM32(g_esp);
        uint32_t wanted = nfs_misc_world_viewport_clear(MEM8(NFS_MISC_MAGAZINE_MODE), clear);
        if (wanted != clear) MEM32(g_esp) = wanted;
        return 1;
    }
    case NFS_MISC_HOOK_BACKGROUND_VIEWPORT: {
        /* [esp]: the clear flag (a pushed 0); esi: the view being set up. */
        uint32_t clear = MEM32(g_esp);
        uint32_t wanted = nfs_misc_background_viewport_clear(MEM8(NFS_MISC_MAGAZINE_MODE), g_esi, clear);
        if (wanted != clear) MEM32(g_esp) = wanted;
        return 1;
    }
    default:
        return 0;
    }
}
