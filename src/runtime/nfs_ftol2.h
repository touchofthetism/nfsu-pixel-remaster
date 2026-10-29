/* nfs_ftol2.h - MSVC's _ftol2 (Speed.exe 0x00674898) as the lifted model computes it, for the host's own
 * versions of the guest code that calls it (overrides.c replaces the function; the layer blend's fast path,
 * nfs_blend.h, does its four conversions a tap inline).
 *
 * The body: fld st0; fst dword [t]; fistp qword [q] (x rounded by the control word; the copy carries no int64
 * shadow); fild qword [q] (r); eax = low(r), edx = bits(t); if eax == 0 { edx = high(r); if edx & 0x7FFFFFFF == 0
 * the result is r }; fsubp st1 (d = x - r, stored as a float); x negative (edx's sign): ecx = bits(d) ^ 0x80000000,
 * r += (ecx > 0x80000000); x positive: ecx = bits(d), r -= (ecx > 0x80000000); ecx += 0x7FFFFFFF. That is r
 * truncated toward zero for every finite value inside int64, in every rounding mode; NaN and out-of-range values
 * give what the model's conversion (fp_st0_to_i64) gives, the same here as in the lifted body.
 *
 * Needs recomp_types.h (fp_st0_to_i64) included first. */
#ifndef NFS_FTOL2_H
#define NFS_FTOL2_H
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t eax, edx;   /* the result, edx:eax */
    uint32_t ecx;        /* ecx as the body leaves it, when ecx_written */
    int ecx_written;     /* 0 on the body's early exit (zero, or the integer indefinite), which keeps ecx */
} nfs_ftol2_result;

static inline nfs_ftol2_result nfs_ftol2(double x, uint16_t cw) {
    nfs_ftol2_result out;
    const float xf = (float)x;
    uint32_t sign_source;                                 /* edx: the float's bits, or the high dword of r */
    memcpy(&sign_source, &xf, 4);
    const int64_t r = fp_st0_to_i64(x, 0, cw);
    const uint32_t r_low = (uint32_t)(uint64_t)r, r_high = (uint32_t)((uint64_t)r >> 32);
    if (r_low == 0) {
        sign_source = r_high;
        if ((r_high & 0x7FFFFFFFu) == 0) {
            out.eax = 0;
            out.edx = r_high;
            out.ecx = 0;
            out.ecx_written = 0;
            return out;
        }
    }
    const float df = (float)(x - (double)r);
    uint32_t d_bits;
    memcpy(&d_bits, &df, 4);
    if ((int32_t)sign_source < 0) {
        d_bits ^= 0x80000000u;
        const uint32_t carry = d_bits > 0x80000000u;      /* add ecx, 0x7FFFFFFF carries */
        out.eax = r_low + carry;
        out.edx = r_high + (out.eax < r_low);             /* adc eax, 0; adc edx, 0 */
    } else {
        const uint32_t borrow = d_bits > 0x80000000u;
        out.eax = r_low - borrow;
        out.edx = r_high - (r_low < borrow);              /* sbb eax, 0; sbb edx, 0 */
    }
    out.ecx = d_bits + 0x7FFFFFFFu;
    out.ecx_written = 1;
    return out;
}

#endif
