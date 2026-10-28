// The decisions of src/runtime/misc_fixes.c (nfs_misc_fixes.h): HD Cursor Support from
// AeroWidescreen's NFSUHDContentSupport and the constants of ThirteenAG's 44.1 kHz fix.
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
    std::puts("misc_fixes: ok");
    return 0;
}
