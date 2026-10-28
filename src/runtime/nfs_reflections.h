/* nfs_reflections.h - sharper reflections for the lifted NFSU1 (see reflections.c).
 *
 * The resolution part of AeroWidescreen's NFS HD Reflections for Speed.exe 1.4
 * (github.com/AeroWidescreen/NFSHDReflections, SOURCE/NFSU, GPL-3.0). The game
 * renders its three reflections into small fixed-size targets that are then
 * stretched across the screen:
 *   - the wet-road mirror image: 320x240, from immediates in the target setup
 *     (CreateTexture 0x40A8B6/0x40A8BB, its depth surface 0x40A8EC/0x40A8F1), the
 *     view table (0x40AACA/0x40AAD1) and the view reset (0x40854C/0x408553);
 *   - the car reflection cube map: edge [0x702A84] = 128 (six faces, six depth
 *     surfaces);
 *   - the rear-view mirror: [0x702A9C] x [0x702AA0] = 256x64.
 * The mod's AutoRes sizes them from the screen (road = the screen, cube = its
 * height, mirror = W/2 x H/6), which is a desktop GPU's budget. Here: the road
 * at a third of the render height with the picture's shape (996x448 on a
 * 2992x1344 panel, 5.8x the pixels), the cube map 256 (4x), the mirror a sixth
 * of the height at its own 4:1 (896x224); never below the originals. The view
 * projections do not depend on these sizes (the widescreen FOV fix uses fixed
 * per-view constants), so only the sampling density changes.
 */
#ifndef NFS_REFLECTIONS_H
#define NFS_REFLECTIONS_H
#include <stdint.h>
#include <math.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Road reflection: `push eax` before the call, width at [esp], height at [esp+4]. */
#define NFS_REFL_HOOK_ROAD_TEXTURE    0x0040A8BFu   /* CreateTexture (vtable +0x5C) */
#define NFS_REFL_HOOK_ROAD_DEPTH      0x0040A8F5u   /* CreateDepthStencilSurface (vtable +0x74) */
/* Road reflection view descriptor: after its two size stores, width at +0x14, height at +0x18. */
#define NFS_REFL_HOOK_ROAD_VIEW_RESET 0x00408557u   /* ecx: mov [ecx+14h],320 / mov [ecx+18h],240 */
#define NFS_REFL_HOOK_ROAD_VIEW_INIT  0x0040AAD5u   /* esi: mov [esi+14h],320 / mov [esi+18h],240 */
#define NFS_REFL_CUBE_EDGE            0x00702A84u
#define NFS_REFL_MIRROR_WIDTH         0x00702A9Cu
#define NFS_REFL_MIRROR_HEIGHT        0x00702AA0u

typedef struct { uint32_t road_w, road_h, cube, mirror_w, mirror_h; } nfs_reflection_sizes;

/* The sizes for a render size and picture aspect (0: render_w / render_h); hd = 0 or no render size: the
 * originals. */
static inline nfs_reflection_sizes nfs_refl_sizes(int hd, uint32_t render_w, uint32_t render_h, float aspect) {
    nfs_reflection_sizes s = { 320u, 240u, 128u, 256u, 64u };
    uint32_t h, w, mh;
    if (!hd || render_w < 320u || render_h < 240u) return s;
    if (aspect <= 0.0f) aspect = (float)render_w / (float)render_h;
    h = (render_h / 3u) & ~1u;
    if (h < 240u) h = 240u;
    w = (uint32_t)lroundf((float)h * aspect) & ~1u;
    if (w < 320u) w = 320u;
    s.road_w = w; s.road_h = h;
    s.cube = 256u;
    mh = (render_h / 6u) & ~1u;
    if (mh < 64u) mh = 64u;
    s.mirror_h = mh; s.mirror_w = 4u * mh;
    return s;
}

/* Before the game runs: hd on/off, the game's render size and picture aspect. */
void nfs_reflections_configure(int hd, unsigned render_w, unsigned render_h, float aspect);
/* Once, after the image is mapped: the cube map and mirror sizes (data the target setup reads). */
void nfs_reflections_boot(void);
/* recomp_hook dispatch: 1 when `va` was one of the road reflection sites. */
int nfs_reflections_hook(uint32_t va);

#ifdef __cplusplus
}
#endif
#endif
