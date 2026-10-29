/*
 * overrides.c - guest functions replaced by hand-written C.
 *
 * Every entry needs a reason that is a fact about the hardware or the host,
 * not "it crashed": an override hides whatever the original did.
 *
 * Calling convention inside the lifted model: the caller has pushed a return
 * slot, arguments are at g_esp+4.., the body leaves its result in g_eax and
 * pops what `ret N` would (g_esp += 4 + N).
 */
#include <string.h>
#include "recomp_types.h"
#include "nfs_runtime.h"
#include "nfs_ftol2.h"

/* 0x00643A39 (SPEED2 0x006F5FC5, same bytes): Cyrix detection.
 *   xor eax,eax; sahf; mov eax,5; mov ebx,2; div bl; lahf; cmp ah,2; ...
 * Cyrix parts leave the flags untouched across DIV, so AH reads back 2 (the
 * value SAHF loaded, plus the always-set bit 1); Intel/AMD do not. DIV's flags
 * are architecturally undefined and the lifter does not model them, so the
 * lifted probe always answers "Cyrix". The host is never a Cyrix: return 0. */
static void ov_is_cyrix(void) {
    g_eax = 0;
    g_esp += 4;
}

/* 0x00672630 and 0x00678D30 (SPEED2 0x0075CFD0, same bytes): the static CRT
 * memcpy/memmove(dst, src, n) (cdecl, returns dst).
 * It handles overlap by copying backward, and reaches its unrolled dword moves
 * through computed jumps into the middle of straight-line blocks (tables at
 * 0x75D11C, 0x75D268, 0x75D2B8...). Not every target is a lifted entry, so an
 * overlapping move with 8+ dwords stopped at "ITAIL unresolved 0x0075D27C".
 * Its contract is exactly the host memmove over guest memory. */
static void ov_crt_memmove(void) {
    uint32_t dst = MEM32(g_esp + 4), src = MEM32(g_esp + 8), n = MEM32(g_esp + 12);
    if (n) memmove((void *)ADDR(dst), (const void *)ADDR(src), n);
    g_eax = dst;
    g_esp += 4;
}

/* 0x00674898: the static CRT's _ftol2 - ST(0) truncated toward zero into edx:eax, ST(0) popped. MSVC calls it for
 * every float-to-integer cast: 784 call sites, 0.6% of the render thread's samples in build 19's 76-minute session,
 * and in the car lot four times per layer of every pixel of a car's composited textures (0x5A8640). Its lifted
 * body pays for a function frame, 22 flag snapshots and five pushes and pops of the x87 window a call; this is the
 * same computation, operation for operation (nfs_ftol2.h), so every value the lifted body produced - NaN and
 * out-of-range inputs included - comes out the same (tests/overrides.cpp holds the body's lifted C beside it).
 * The stack below the caller's esp that the body used as scratch is dead after its ret, and no caller reads the
 * flags it leaves: in Speed.exe's disassembly every one of the 784 call sites sets them again before any
 * conditional jump, set, cmov, adc or sbb. */
static void ov_crt_ftol2(void) {
    const nfs_ftol2_result r = nfs_ftol2(g_st[0], g_fpu_cw);
    (void)fp_pop_impl(g_st, &g_fp_top);               /* x leaves the stack, as after the body's last pop */
    g_eax = r.eax;
    g_edx = r.edx;
    if (r.ecx_written) g_ecx = r.ecx;
    g_esp += 4;
}

static const struct { uint32_t va; nfs_override_fn fn; } k_overrides[] = {
    { 0x00674898u, ov_crt_ftol2 },   /* first: by far the most called */
    { 0x00643A39u, ov_is_cyrix },
    { 0x00672630u, ov_crt_memmove },
    { 0x00678D30u, ov_crt_memmove },
};

nfs_override_fn nfs_override_lookup(uint32_t va) {
    for (unsigned i = 0; i < sizeof k_overrides / sizeof k_overrides[0]; i++)
        if (k_overrides[i].va == va) return k_overrides[i].fn;
    return 0;
}

int nfs_override_count(void) { return (int)(sizeof k_overrides / sizeof k_overrides[0]); }
