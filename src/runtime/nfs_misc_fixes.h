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

/* HD Font Support (AeroWidescreen's NFSUHDContentSupport, GPL-3.0). Text is drawn from glyph tables in the
 * stock font textures' pixels: the text and glyph functions read the font texture's width and height from its
 * record (+0x44, +0x46) to turn those pixels into texture coordinates, 0x40BF40 at 0x40C0E8-0x40C0F3 (record
 * in ecx; width to edx and [esp+0Ch], height to eax) and 0x4F4790 at 0x4F479B-0x4F47A9 (record at [edi+8];
 * width to [esp+4], height to edx). An HD font (Plajer's HD User Interface: 2x the stock size) would then be
 * sampled at the wrong places. The mod jumps over those loads with the stock size for nine font textures;
 * here a hook right after them (0x40C0F4 fild [esp+0Ch], 0x4F47AA fild [esp+4]: neither a branch target in
 * the catalog) puts the stock size where the loads left the real one. The sizes are the stock 1.4 data's
 * (checked against Languages/LanguageTextures.bin and Global/GLOBALA.BUN): the mod has
 * CONDUITMDITC_TT21I_KOREAN as 512 wide and 1024 high, which is the wrong way round for the stock 1024x512
 * texture and would garble Korean text even with the stock font; it is 1024x512 here. Any other texture,
 * and a stock font (its stock size is its size), is left as loaded. */
#define NFS_MISC_HOOK_FONT_SIZE_TEXT   0x0040C0F4u   /* record ecx: edx, [esp+0Ch] width; eax height */
#define NFS_MISC_HOOK_FONT_SIZE_GLYPH  0x004F47AAu   /* record [edi+8]: [esp+4] width; edx height */

/* 1 and the stock size for the nine font textures (record +0x24 hash), 0 for any other texture. */
static inline int nfs_misc_font_stock_size(uint32_t texture_hash, uint32_t* width, uint32_t* height) {
    switch (texture_hash) {
    case 0x5B9D88B9u:   /* CONDUITMDITC_TT21I (Global/GLOBALA.BUN) */
    case 0x5B9D84DBu:   /* CONDUITMDITC_TT14I */
    case 0xA87927BEu:   /* FONT_ARIAL */
    case 0xAB6215C1u:   /* FONT_ARIAL12 */
        *width = 256; *height = 256; return 1;
    case 0xDCA5485Au:   /* CONDUITMDITC_TT14I_KOREAN */
    case 0x9583AA1Au:   /* FONT_CONDUITMDITCTT38BI */
    case 0x0920075Cu:   /* FONT_IMPACT36 */
        *width = 512; *height = 512; return 1;
    case 0x833A8678u:   /* CONDUITMDITC_TT21I_KOREAN */
        *width = 1024; *height = 512; return 1;
    case 0xBBBA71C2u:   /* LCD_LET48 */
        *width = 128; *height = 128; return 1;
    default:
        return 0;
    }
}

/* Magazine covers: ThirteenAG's BlackMagazineFix (NFSUnderground.WidescreenFix,
 * BlackMagazineFix.ixx). With Light Trails or Motion Blur on, the world is drawn
 * through the effect target and [0x73645C] is 1, so the 3D view's setup
 * (0x40ABD0: render target, depth, then Clear to black when its pushed flag is
 * set) clears the screen and wipes the cover's background, drawn before it. In
 * the front end's magazine view (byte 0x735768) the 3D view does not clear and
 * the background view (0x71AA4C) clears instead; elsewhere nothing changes.
 * Both hooks sit on the call to 0x40ABD0 with the clear flag at [esp]: the 3D
 * view's at 0x4098B4 (after cmp [0x73645C],1 / jne / push 1 or push 0) and the
 * background's at 0x409CF7 (after push 0 and esi = 0x71AA30 or 0x71AA4C). */
#define NFS_MISC_MAGAZINE_MODE            0x00735768u   /* byte: nonzero while the magazine view is up */
#define NFS_MISC_HOOK_WORLD_VIEWPORT      0x004098B4u
#define NFS_MISC_HOOK_BACKGROUND_VIEWPORT 0x00409CF7u
#define NFS_MISC_BACKGROUND_VIEWPORT      0x0071AA4Cu

/* The clear flag to leave at [esp] for the 3D view. */
static inline uint32_t nfs_misc_world_viewport_clear(uint8_t magazine, uint32_t clear) {
    return magazine ? 0u : clear;
}
/* The clear flag to leave at [esp] for the view in esi. */
static inline uint32_t nfs_misc_background_viewport_clear(uint8_t magazine, uint32_t viewport, uint32_t clear) {
    return magazine && viewport == NFS_MISC_BACKGROUND_VIEWPORT ? 1u : clear;
}

/* Once, after the image is mapped and before the game's entry point runs. */
void nfs_misc_fixes_boot(void);
/* recomp_hook dispatch: 1 when `va` was one of these fixes' sites. */
int nfs_misc_hook(uint32_t va);

#ifdef __cplusplus
}
#endif
#endif
