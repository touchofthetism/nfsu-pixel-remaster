// The decisions of src/runtime/misc_fixes.c (nfs_misc_fixes.h): HD Cursor Support from
// AeroWidescreen's NFSUHDContentSupport, ThirteenAG's BlackMagazineFix and the constants of his 44.1 kHz fix.
#include "../src/runtime/nfs_misc_fixes.h"
#include <cstdio>
#include <cstdlib>
static void require(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
int main() {
    // Record +0x44 holds width (low 16 bits) and height (high 16 bits).
    const uint32_t hd = 64u | (64u << 16), stock = 16u | (16u << 16);
    require(nfs_misc_cursor_size(0x6B6A294Fu, hd) == stock, "an HD PC_CURSOR is drawn at the stock 16x16");
    require(nfs_misc_cursor_size(0x6B6A294Fu, stock) == stock, "the stock cursor is left as it is");
    require(nfs_misc_cursor_size(0x12345678u, hd) == hd, "any other texture keeps its size");
    require(NFS_MISC_CURSOR_DRAW_SIZE == 0x00100010u, "width 16 and height 16 as AeroWidescreen writes them");
    require(NFS_MISC_HOOK_CURSOR_SIZE == 0x004F36B6u, "the hook sits on movsx ecx, word [edi+44h] in the arrow draw");
    require(NFS_MISC_SOUND_RATE_OVERRIDE == 0x00735610u && NFS_MISC_FULL_SOUND_RATE == 44100u, "ThirteenAG's override cell and rate");
    // BlackMagazineFix: the 3D view never clears in the magazine view; the background view (0x71AA4C) does.
    require(nfs_misc_world_viewport_clear(0, 1) == 1 && nfs_misc_world_viewport_clear(0, 0) == 0, "outside the magazine view the 3D view clears as the game decided");
    require(nfs_misc_world_viewport_clear(1, 1) == 0 && nfs_misc_world_viewport_clear(1, 0) == 0, "in the magazine view the 3D view leaves the background drawn before it");
    require(nfs_misc_background_viewport_clear(1, 0x0071AA4Cu, 0) == 1, "in the magazine view the background view clears instead");
    require(nfs_misc_background_viewport_clear(1, 0x0071AA30u, 0) == 0, "the other view set up at the same call keeps its flag");
    require(nfs_misc_background_viewport_clear(0, 0x0071AA4Cu, 0) == 0, "outside the magazine view the background view keeps the game's flag");
    require(NFS_MISC_HOOK_WORLD_VIEWPORT == 0x004098B4u && NFS_MISC_HOOK_BACKGROUND_VIEWPORT == 0x00409CF7u && NFS_MISC_MAGAZINE_MODE == 0x00735768u,
            "the calls to 0x40ABD0 after the mod's two patterns, and its magazine flag, in Speed.exe 1.4");
    std::puts("misc_fixes: ok");
    return 0;
}
