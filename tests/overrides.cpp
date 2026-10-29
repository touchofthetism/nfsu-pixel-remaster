// The hand-written guest functions (src/runtime/overrides.c) against the lifted code they replace.
//
// _ftol2 (0x00674898): the override must leave exactly what its lifted body left - edx:eax, ecx, the x87 window
// (one value popped, the registers below unchanged with their int64 shadows) - for every double in every rounding
// mode, NaN, infinities and out-of-range values included (their conversions are the platform's, the same for
// both because both go through the model's fp_st0_to_i64). The reference below is the body as the lifter emits
// it, an x87 instruction at a time, through the same recomp_types.h primitives, on a guest stack.
#define RECOMP_X87_WINDOW
extern "C" {
#include "recomp_types.h"
#include "../src/runtime/overrides.c"
}
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

extern "C" {
uint32_t g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_ebp, g_esp;
ptrdiff_t g_mem_base;
int g_fp_top;
uint16_t g_fpu_cw = 0x027F;
}

static int failures = 0;
static long checks = 0;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { if (++failures <= 20) { std::fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } } } while (0)

static uint8_t g_guest[0x10000];            // guest addresses 0x1000.. map here (the stack)
static constexpr uint32_t kStackTop = 0x8000;

static uint32_t mem32(uint32_t va) { uint32_t v; std::memcpy(&v, (void*)ADDR(va), 4); return v; }
static void set32(uint32_t va, uint32_t v) { std::memcpy((void*)ADDR(va), &v, 4); }
static void setFloat(uint32_t va, float f) { std::memcpy((void*)ADDR(va), &f, 4); }

// _ftol2's lifted body (Speed.exe 0x00674898, MSVC's CRT): one statement per instruction, as run_lift.py emits it.
static void lifted_ftol2() {
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, esp = g_esp, ebp = g_ebp;
    esp -= 4; set32(esp, ebp);                                                      // push ebp
    ebp = esp;                                                                      // mov ebp, esp
    esp -= 0x20;                                                                    // sub esp, 20h
    esp &= 0xFFFFFFF0u;                                                             // and esp, -16
    fp_push_impl(g_st, &g_fp_top, g_st[0]);                                         // fld st(0)
    setFloat(esp + 0x18, (float)g_st[0]);                                           // fst dword [esp+18h]
    { int64_t q = fp_st0_to_i64(g_st[0], g_st_i64[0], g_fpu_cw); (void)fp_pop_impl(g_st, &g_fp_top);
      std::memcpy((void*)ADDR(esp + 0x10), &q, 8); }                                // fistp qword [esp+10h]
    { int64_t q; std::memcpy(&q, (void*)ADDR(esp + 0x10), 8);
      fp_push_impl(g_st, &g_fp_top, (double)q); g_st_i64[0] = q; }                  // fild qword [esp+10h]
    edx = mem32(esp + 0x18);                                                        // mov edx, [esp+18h]
    eax = mem32(esp + 0x10);                                                        // mov eax, [esp+10h]
    if (eax == 0) {                                                                 // test eax, eax; je
        edx = mem32(esp + 0x14);                                                    // mov edx, [esp+14h]
        if ((edx & 0x7FFFFFFFu) == 0) {                                             // test edx, 7FFFFFFFh; jne
            setFloat(esp + 0x18, (float)fp_pop_impl(g_st, &g_fp_top));              // fstp dword [esp+18h]
            setFloat(esp + 0x18, (float)fp_pop_impl(g_st, &g_fp_top));              // fstp dword [esp+18h]
            goto leave;
        }
    }
    { double v = fp_pop_impl(g_st, &g_fp_top); g_st[0] -= v; }                      // fsubp st(1), st
    if ((int32_t)edx < 0) {                                                         // test edx, edx; jns
        setFloat(esp, (float)fp_pop_impl(g_st, &g_fp_top));                         // fstp dword [esp]
        ecx = mem32(esp);                                                           // mov ecx, [esp]
        ecx ^= 0x80000000u;                                                         // xor ecx, 80000000h
        uint32_t cf = (uint64_t)ecx + 0x7FFFFFFFu > 0xFFFFFFFFu; ecx += 0x7FFFFFFFu; // add ecx, 7FFFFFFFh
        uint64_t s = (uint64_t)eax + cf; eax = (uint32_t)s; cf = uint32_t(s >> 32); // adc eax, 0
        edx = mem32(esp + 0x14);                                                    // mov edx, [esp+14h]
        edx += cf;                                                                  // adc edx, 0
    } else {
        setFloat(esp, (float)fp_pop_impl(g_st, &g_fp_top));                         // fstp dword [esp]
        ecx = mem32(esp);                                                           // mov ecx, [esp]
        uint32_t cf = (uint64_t)ecx + 0x7FFFFFFFu > 0xFFFFFFFFu; ecx += 0x7FFFFFFFu; // add ecx, 7FFFFFFFh
        uint32_t borrow = eax < cf; eax -= cf; cf = borrow;                         // sbb eax, 0
        edx = mem32(esp + 0x14);                                                    // mov edx, [esp+14h]
        edx -= cf;                                                                  // sbb edx, 0
    }
leave:
    esp = ebp; ebp = mem32(esp); esp += 4;                                          // leave
    esp += 4;                                                                       // ret
    g_eax = eax; g_ecx = ecx; g_edx = edx; g_esp = esp; g_ebp = ebp;
}

struct X87 { std::vector<double> st; std::vector<int64_t> shadow; int top; };
static X87 snapshot() {
    X87 s; s.top = g_fp_top;
    for (int i = 0; i < 8; ++i) { s.st.push_back(g_st[i]); s.shadow.push_back(g_st_i64[i]); }
    return s;
}
static bool same(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }   // NaN payloads and -0 too

// One value through both: the stack holds seven earlier values (some with int64 shadows, as fild leaves them).
static void compare(double x, uint16_t cw, const char* what) {
    auto load = [&] {
        g_fp_sp = RECOMP_X87_SLOTS / 2; g_fp_top = 0;
        for (int i = 0; i < 7; ++i) {
            fp_push_impl(g_st, &g_fp_top, 1000.0 + i);
            if (i % 2) g_st_i64[0] = 1000 + i;
        }
        fp_push_impl(g_st, &g_fp_top, x);
        g_fpu_cw = cw;
        g_esp = kStackTop - 4; set32(g_esp, 0xDEADBEEF);    // the return slot RECOMP_CALL pushed
        g_eax = 0x11111111; g_ecx = 0x22222222; g_edx = 0x33333333; g_ebp = 0x44444444;
    };
    load();
    lifted_ftol2();
    const uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, esp = g_esp, ebp = g_ebp;
    const X87 after = snapshot();
    load();
    ov_crt_ftol2();
    const X87 got = snapshot();
    CHECK(g_eax == eax && g_edx == edx, "%s x=%.17g cw=%04x: edx:eax %08x:%08x, lifted %08x:%08x", what, x, cw, g_edx, g_eax, edx, eax);
    CHECK(g_ecx == ecx, "%s x=%.17g cw=%04x: ecx %08x, lifted %08x", what, x, cw, g_ecx, ecx);
    CHECK(g_esp == esp && g_ebp == ebp, "%s x=%.17g: esp %08x ebp %08x, lifted %08x %08x", what, x, g_esp, g_ebp, esp, ebp);
    CHECK(got.top == after.top, "%s x=%.17g: top %d, lifted %d", what, x, got.top, after.top);
    for (int i = 0; i < 7; ++i) {   // ST(7) is empty after the pop: its stale slot is not part of the state
        CHECK(same(got.st[size_t(i)], after.st[size_t(i)]) && got.shadow[size_t(i)] == after.shadow[size_t(i)],
              "%s x=%.17g: ST(%d) %.17g/%lld, lifted %.17g/%lld", what, x, i, got.st[size_t(i)],
              (long long)got.shadow[size_t(i)], after.st[size_t(i)], (long long)after.shadow[size_t(i)]);
    }
    // The contract, where it has one: a finite value inside int64 is truncated toward zero, in every mode.
    if (std::isfinite(x) && std::fabs(x) < 9223372036854775808.0) {
        const int64_t result = int64_t((uint64_t(g_edx) << 32) | g_eax);
        CHECK(result == int64_t(std::trunc(x)), "%s x=%.17g cw=%04x: %lld, trunc %lld", what, x, cw, (long long)result, (long long)std::trunc(x));
    }
}

int main() {
    g_mem_base = reinterpret_cast<ptrdiff_t>(g_guest) - 0x1000;
    const uint16_t modes[] = {0x027F, 0x067F, 0x0A7F, 0x0E7F, 0x007F};   // nearest, down, up, chop (53- and 24-bit precision words)
    std::vector<double> values = {
        0.0, -0.0, 0.25, -0.25, 0.5, -0.5, 0.75, -0.75, 1.0, -1.0, 1.5, -1.5, 2.5, -2.5, 3.5, -3.5,
        0.49999999999999994, -0.49999999999999994, 0.5000000000000001, -0.5000000000000001, 0.9999999999999999,
        2147483647.0, -2147483648.0, 2147483647.5, -2147483648.5, 2147483648.0, 4294967295.5, 4294967296.0,
        -4294967296.0, 4294967297.0, -4294967297.5, 4503599627370495.5, -4503599627370495.5, 4503599627370496.0,
        9007199254740993.0, 4611686018427387904.0, -4611686018427387904.0, 9223372036854774784.0,
        -9223372036854775808.0, 9223372036854775808.0, -9223372036854777856.0, 1e19, -1e19, 1e300, -1e300,
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(), -std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::denorm_min(), -std::numeric_limits<double>::denorm_min(), 1e-310, -1e-310,
        1e-45, -1e-45, 1.401298464324817e-45, 255.0 * 0.0625, 254.99999999999997, 255.00000000000003,
    };
    for (double v : values)
        for (uint16_t cw : modes) compare(v, cw, "special");
    // The products the car lot's filter converts (a byte times a float weight), and values all over the range.
    std::mt19937_64 random(20261029);
    for (int i = 0; i < 300000; ++i) {
        double v;
        switch (i % 4) {
        case 0: v = double(random() % 256) * double(float(std::ldexp(double(random() % (1u << 24)), -24))); break;
        case 1: v = std::ldexp(double(int64_t(random() >> 11)) / 9007199254740992.0, int(random() % 140) - 70); break;
        case 2: v = double(int64_t(random() % 2000001) - 1000000) + (random() % 2 ? 0.5 : -0.5); break;
        default: { uint64_t bits = random(); std::memcpy(&v, &bits, 8); break; }   // any bit pattern
        }
        if (random() % 2) v = -v;
        compare(v, modes[size_t(i) % 5], "random");
    }
    // The table: ftol2 is found first (the dispatcher scans it on every call).
    CHECK(nfs_override_lookup(0x00674898u) == ov_crt_ftol2, "ftol2 registered");
    CHECK(nfs_override_lookup(0x00672630u) == ov_crt_memmove && nfs_override_lookup(0x00678D30u) == ov_crt_memmove, "memmove");
    CHECK(nfs_override_lookup(0x00643A39u) == ov_is_cyrix && nfs_override_lookup(0x00401000u) == nullptr, "others");
    CHECK(nfs_override_count() == 4, "four overrides");
    std::printf("overrides: %ld checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
