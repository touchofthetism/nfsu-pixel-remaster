/* nfs_misc_fixes.h - small PC-community fixes ported to the lifted game (see misc_fixes.c).
 *
 * Addresses are in Speed.exe 1.4 and were found by the original mods' byte
 * patterns (ThirteenAG's NFSUnderground.WidescreenFix, Misc.ixx) or taken from
 * AeroWidescreen's NFSUHDContentSupport (which checks for 1.4 by entry point).
 * The decisions are header-only so tests/misc_fixes.cpp can exercise them.
 */
#ifndef NFS_MISC_FIXES_H
#define NFS_MISC_FIXES_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Sound library sample rate. The sound init (0x532A20, again at 0x532BB0)
 * opens the mixer at 22050 Hz when the performance tier ([0x736334], the
 * .cfg's PERFORMANCE) is 0 or 1 and at 44100 Hz at tiers 2-3, then replaces
 * that with [0x735610] when it is nonzero. Nothing in the game writes
 * 0x735610 (two reads, at 0x532B15 and 0x532C3D; zero-initialised .data), so
 * ThirteenAG's fix sets it to 44100 once, before the game runs. */
#define NFS_MISC_SOUND_RATE_OVERRIDE   0x00735610u
#define NFS_MISC_SOUND_RATE            0x006F7E88u   /* the rate the mixer was opened with */
#define NFS_MISC_FULL_SOUND_RATE       44100u

/* The front-end arrow (draw function 0x4F3030) is sized from its texture's
 * stored width and height (record +0x44, two 16-bit values): the stock
 * PC_CURSOR is 16x16, and an HD one is drawn that many units larger. The HD
 * packs ship a 64x64 PC_CURSOR in FrontEnd/FrontB.lzc. AeroWidescreen's HD
 * Cursor Support rewrites the size in the in-memory record to 16x16 right
 * before the read (0x4F36B6: movsx ecx, word [edi+44h]) so the sharp texture
 * is drawn at the original size; the D3D texture was created at load and
 * keeps its real size. */
#define NFS_MISC_HOOK_CURSOR_SIZE      0x004F36B6u
#define NFS_MISC_PC_CURSOR_HASH        0x6B6A294Fu   /* record +0x24 */
#define NFS_MISC_CURSOR_DRAW_SIZE      0x00100010u   /* width 16, height 16 (record +0x44, +0x46) */

/* The size word pair to leave in the record: 16x16 for PC_CURSOR, anything else untouched. */
static inline uint32_t nfs_misc_cursor_size(uint32_t texture_hash, uint32_t size) {
    return texture_hash == NFS_MISC_PC_CURSOR_HASH ? NFS_MISC_CURSOR_DRAW_SIZE : size;
}

/* Once, after the image is mapped and before the game's entry point runs. */
void nfs_misc_fixes_boot(void);
/* recomp_hook dispatch: 1 when `va` was one of these fixes' sites. */
int nfs_misc_hook(uint32_t va);

#ifdef __cplusplus
}
#endif
#endif
