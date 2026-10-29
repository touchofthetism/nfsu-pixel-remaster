/* nfs_blend.h - the layer blend that composites a car's textures (Speed.exe 0x005A8640), done by the host.
 *
 * When a car is shown with its paint and vinyls (the car lot, the shops, a race start), the game composites the
 * car's textures on the CPU, and every pixel of them goes through 0x5A8640 (cdecl):
 *
 *     uint32_t blend(const uint8_t* pixels, const float* weights, int count, bool alphaMax)
 *
 * `count` BGRA pixels (the layers; the callers pass 2 to 4) each weighted (a layer's alpha over 255 as the
 * callers compute it): a layer weighing no more than [0x6CC970] (1/255) is skipped; for the others each colour
 * channel adds (int)(byte * weight), converted by _ftol2; alpha is the largest such value (its low byte) when
 * alphaMax, else the sum; each channel is clamped to 255 and the four packed as the layers are. The body runs
 * the layers four at a time, then one at a time, through x87 code and 4 calls of _ftol2 a layer: in build 19's
 * session each car the player looked at in the car lot cost 80-100 ms of it on the render thread (two thirds of
 * the hitch: 0x5A8640 and _ftol2 at the top of every slow frame there); about 19,000 calls a car (the test host
 * counted 321,122 for 17 cars, with the stock data and with the HD packs alike).
 *
 * nfs_layer_blend computes what the body leaves - the result in eax, the blue sum in ecx as its last load leaves
 * it, edx as its last write leaves it, and the one stack slot it writes that the caller owns (its first
 * argument's, which the one-at-a-time loop uses as scratch) - with the lifted model's own arithmetic (each
 * product a double of the byte and the float weight; _ftol2 as nfs_ftol2.h). The x87 stack is left as it
 * was: every value the body pushes it pops. The flags it leaves are not read by its callers (each adds to esp
 * first). tests/layer_blend.cpp checks the arithmetic; the test host's verify mode runs the lifted body beside
 * it on every call of a session (NFS_VERIFY_FASTPATHS).
 *
 * Needs recomp_types.h (fp_st0_to_i64) included first. */
#ifndef NFS_BLEND_H
#define NFS_BLEND_H
#include <stdint.h>
#include <string.h>
#include "nfs_ftol2.h"

#define NFS_LAYER_BLEND            0x005A8640u
#define NFS_LAYER_BLEND_THRESHOLD  0x006CC970u   /* float: a layer weighing this (1/255) or less is skipped */

typedef struct {
    uint32_t eax;         /* the blended pixel */
    uint32_t ecx;         /* the blue sum before clamping (the body's last load of ecx) */
    uint32_t edx;         /* edx as the body leaves it, when edx_written */
    int edx_written;      /* 0: the body did not write edx (no layer used, no one-at-a-time loop) */
    uint32_t arg0;        /* what the one-at-a-time loop left in its first argument's stack slot ... */
    int arg0_written;     /* ... when it used a layer */
} nfs_layer_blend_result;

static inline uint8_t nfs_layer_blend_clamp(uint32_t sum) { return (int32_t)sum > 255 ? 255 : (uint8_t)sum; }

/* pixels: 4 * count bytes; weights: 4 * count bytes (floats, any alignment); pixels_va: the first argument as the
 * guest passed it (edx holds it when the one-at-a-time loop starts). */
static inline nfs_layer_blend_result nfs_layer_blend(const uint8_t* pixels, const uint8_t* weights, int32_t count,
                                                     uint8_t alpha_max, float threshold, uint16_t cw, uint32_t pixels_va) {
    nfs_layer_blend_result out;
    uint32_t sum2 = 0, sum1 = 0, sum0 = 0, alpha = 0;     /* [esp+10h] (byte 2), [esp+14h] (byte 1), [esp+18h], ebx */
    out.edx = 0; out.edx_written = 0; out.arg0 = 0; out.arg0_written = 0;
    const int32_t unrolled = count >= 4 ? 4 + (count - 4) / 4 * 4 : 0;   /* blocks of four while 4k+3 < count */
    for (int32_t i = 0; i < count; ++i) {
        if (i == unrolled) { out.edx = pixels_va; out.edx_written = 1; }  /* mov edx, [esp+2Ch] before the loop */
        float w;
        memcpy(&w, weights + 4 * (size_t)i, 4);
        if (!((double)w > (double)threshold)) continue;   /* fcomp: below, equal or unordered skips the layer */
        const uint8_t* p = pixels + 4 * (size_t)i;
        sum2 += nfs_ftol2((double)(int32_t)p[2] * (double)w, cw).eax;
        sum1 += nfs_ftol2((double)(int32_t)p[1] * (double)w, cw).eax;
        sum0 += nfs_ftol2((double)(int32_t)p[0] * (double)w, cw).eax;
        const nfs_ftol2_result a = nfs_ftol2((double)(int32_t)p[3] * (double)w, cw);
        if (alpha_max) { const uint32_t low = a.eax & 0xFFu; if ((int32_t)low > (int32_t)alpha) alpha = low; }
        else alpha += a.eax;
        out.edx = a.edx;                                  /* the layer's last _ftol2 */
        out.edx_written = 1;
        if (i >= unrolled) { out.arg0 = p[3]; out.arg0_written = 1; }   /* its last scratch store: alpha's byte */
    }
    out.eax = (uint32_t)nfs_layer_blend_clamp(sum0) | (uint32_t)nfs_layer_blend_clamp(sum1) << 8 |
              (uint32_t)nfs_layer_blend_clamp(sum2) << 16 | (uint32_t)nfs_layer_blend_clamp(alpha) << 24;
    out.ecx = sum0;
    return out;
}

#endif
