/*
 * reflections.c - sharper reflection targets (nfs_reflections.h).
 *
 * AeroWidescreen's NFS HD Reflections (NFSU, GPL-3.0) writes the road size into
 * the x86 immediates and the cube/mirror sizes into .data. The game here is
 * lifted C, so the immediates become hooks right after the pushes or stores
 * that carry them, and the data is written once before the game runs. The
 * mod's other options (reflection LOD, skybox in reflections, road reflection
 * inside car and mirror reflections, chrome, tunnel distance) are not ported:
 * they draw more, and the CPU, not the GPU, sets this port's frame rate.
 *
 * Each site checks for the original value before replacing it, so a profile
 * that never reaches the code (reflections off in the game) or a different
 * executable leaves the game untouched.
 *
 * Shared by the Windows host and the Android runtime.
 */
#include <stdint.h>
#include "recomp_types.h"
#include "nfs_reflections.h"

static nfs_reflection_sizes s_sizes = { 320u, 240u, 128u, 256u, 64u };

void nfs_reflections_configure(int hd, unsigned render_w, unsigned render_h, float aspect) {
    s_sizes = nfs_refl_sizes(hd, render_w, render_h, aspect);
}

void nfs_reflections_boot(void) {
    if (MEM32(NFS_REFL_CUBE_EDGE) == 128u) MEM32(NFS_REFL_CUBE_EDGE) = s_sizes.cube;
    if (MEM32(NFS_REFL_MIRROR_WIDTH) == 256u && MEM32(NFS_REFL_MIRROR_HEIGHT) == 64u) {
        MEM32(NFS_REFL_MIRROR_WIDTH) = s_sizes.mirror_w;
        MEM32(NFS_REFL_MIRROR_HEIGHT) = s_sizes.mirror_h;
    }
}

static void resize_pair(uint32_t width_va, uint32_t height_va) {
    if (MEM32(width_va) == 320u && MEM32(height_va) == 240u) {
        MEM32(width_va) = s_sizes.road_w;
        MEM32(height_va) = s_sizes.road_h;
    }
}

int nfs_reflections_hook(uint32_t va) {
    switch (va) {
    case NFS_REFL_HOOK_ROAD_TEXTURE:
    case NFS_REFL_HOOK_ROAD_DEPTH:
        resize_pair(g_esp, g_esp + 4u);
        return 1;
    case NFS_REFL_HOOK_ROAD_VIEW_RESET:
        resize_pair(g_ecx + 0x14u, g_ecx + 0x18u);
        return 1;
    case NFS_REFL_HOOK_ROAD_VIEW_INIT:
        resize_pair(g_esi + 0x14u, g_esi + 0x18u);
        return 1;
    default:
        return 0;
    }
}
