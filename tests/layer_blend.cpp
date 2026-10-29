// The car textures' layer blend done by the host (src/runtime/nfs_blend.h) against what Speed.exe's 0x5A8640 does.
//
// The pixel: every layer weighing more than 1/255 adds (int)(byte * weight) to its channel sums (alpha the largest
// such value's low byte, or the sum), each channel clamped to 255 - checked against a plain model of that over
// random layers, the weights the callers make (a byte over 255) and odd ones (exactly the threshold, NaN, infinite,
// negative). Then what else the body leaves, case by case: ecx (the blue sum), edx (the last layer's _ftol2, or
// the pixels pointer the one-at-a-time loop loads), the first argument's stack slot that loop uses as scratch.
//
// Built with -DNFS_TEST_LIFTED_BLEND and linked with the lifted bodies of 0x5A8640 and 0x674898 (generated from
// the game, so not in this repository), it also runs the game's own code on the same state and compares every
// register, the stack slot and the x87 stack.
#define RECOMP_X87_WINDOW
extern "C" {
#include "recomp_types.h"
#include "nfs_blend.h"
}
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

extern "C" {
uint32_t g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_ebp, g_esp, g_cur_func;
ptrdiff_t g_mem_base;
int g_fp_top;
uint16_t g_fpu_cw = 0x027F;
}

static int failures = 0;
static long checks = 0;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { if (++failures <= 20) { std::fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } } } while (0)

static const float kThreshold = 0.003921568859368563f;   // [0x6CC970]: 1/255 as a float

// The pixel as the game means it.
static uint32_t model(const std::vector<uint8_t>& pixels, const std::vector<float>& weights, bool alphaMax) {
    int64_t sum[3] = {0, 0, 0}, alpha = 0;
    for (size_t i = 0; i < weights.size(); ++i) {
        const double w = weights[i];
        if (!(w > double(kThreshold))) continue;
        for (int c = 0; c < 3; ++c) sum[c] += int64_t(std::trunc(double(pixels[4 * i + size_t(c)]) * w));
        const int64_t a = int64_t(std::trunc(double(pixels[4 * i + 3]) * w));
        if (alphaMax) alpha = std::max<int64_t>(alpha, a & 0xFF); else alpha += a;
    }
    auto clamp = [](int64_t v) { return uint32_t(int32_t(uint32_t(v)) > 255 ? 255 : uint8_t(v)); };
    return clamp(sum[0]) | clamp(sum[1]) << 8 | clamp(sum[2]) << 16 | clamp(alpha) << 24;
}

#ifdef NFS_TEST_LIFTED_BLEND
extern "C" void sub_005A8640_lifted(void);
static uint8_t g_guest[0x700000];   // guest addresses map one to one (the threshold lives at 0x6CC970)
// The game's code on a guest stack: the four arguments pushed, the return slot, the layers and weights in guest
// memory, the x87 stack holding three values; then every register it leaves, its first argument's slot, x87.
static void compareLifted(const std::vector<uint8_t>& pixels, const std::vector<float>& weights, bool alphaMax, uint16_t cw) {
    const uint32_t pixelsVa = 0x2000, weightsVa = 0x3000, esp = 0x8000 - 20;
    std::memcpy(g_guest + pixelsVa, pixels.data(), pixels.size());
    std::memcpy(g_guest + weightsVa, weights.data(), weights.size() * 4);
    const int32_t count = int32_t(weights.size());
    auto put = [](uint32_t va, uint32_t v) { std::memcpy(g_guest + va, &v, 4); };
    auto get = [](uint32_t va) { uint32_t v; std::memcpy(&v, g_guest + va, 4); return v; };
    put(esp, 0xDEADBEEF); put(esp + 4, pixelsVa); put(esp + 8, weightsVa); put(esp + 12, uint32_t(count)); put(esp + 16, alphaMax ? 0x77777701u : 0x77777700u);
    std::memcpy(g_guest + NFS_LAYER_BLEND_THRESHOLD, &kThreshold, 4);
    g_fp_sp = RECOMP_X87_SLOTS / 2; g_fp_top = 0;
    for (int i = 0; i < 3; ++i) fp_push_impl(g_st, &g_fp_top, 7.0 + i);
    g_fpu_cw = cw;
    g_esp = esp; g_eax = 0x11111111; g_ecx = 0x22222222; g_edx = 0x33333333; g_ebx = 0x44444444; g_esi = 0x55555555; g_edi = 0x66666666; g_ebp = 0x77777777;
    sub_005A8640_lifted();
    const nfs_layer_blend_result r = nfs_layer_blend(pixels.data(), reinterpret_cast<const uint8_t*>(weights.data()), count, alphaMax ? 1 : 0, kThreshold, cw, pixelsVa);
    CHECK(g_eax == r.eax && g_ecx == r.ecx, "count %d max %d: eax %08x ecx %08x, host %08x %08x", count, alphaMax, g_eax, g_ecx, r.eax, r.ecx);
    CHECK(g_edx == (r.edx_written ? r.edx : 0x33333333u), "count %d: edx %08x, host %08x (%d)", count, g_edx, r.edx, r.edx_written);
    CHECK(get(esp + 4) == (r.arg0_written ? r.arg0 : pixelsVa), "count %d: arg0 %08x, host %08x (%d)", count, get(esp + 4), r.arg0, r.arg0_written);
    CHECK(g_esp == esp + 4 && g_ebx == 0x44444444u && g_esi == 0x55555555u && g_edi == 0x66666666u && g_ebp == 0x77777777u, "callee-saved registers and esp");
    CHECK(g_fp_top == 3 && g_st[0] == 9.0 && g_st[1] == 8.0 && g_st[2] == 7.0, "x87 stack as it was");
}
#endif

int main() {
#ifdef NFS_TEST_LIFTED_BLEND
    g_mem_base = reinterpret_cast<ptrdiff_t>(g_guest);
#endif
    std::mt19937_64 random(0x5A8640);
    const uint16_t modes[] = {0x027F, 0x0E7F, 0x067F, 0x0A7F};
    long blended = 0;
    for (int round = 0; round < 200000; ++round) {
        const int count = int(random() % 7);   // 0-6: no layer, the one-at-a-time loop alone, four at a time and the rest
        std::vector<uint8_t> pixels(size_t(count) * 4);
        std::vector<float> weights(static_cast<size_t>(count));
        for (auto& p : pixels) p = uint8_t(random());
        for (auto& w : weights) {
            switch (random() % 8) {
            case 0: w = 0.0f; break;
            case 1: w = kThreshold; break;                                        // exactly: skipped
            case 2: w = std::nextafter(kThreshold, 1.0f); break;                  // just above: used
            case 3: w = float(random() % 256) * kThreshold; break;                // as the callers make them
            case 4: w = float(std::ldexp(double(random() % (1u << 24)), -24)) * 2.0f; break;
            default: w = float(random() % 256) * kThreshold; break;
            }
        }
        const bool alphaMax = random() % 2;
        const uint16_t cw = modes[size_t(round) % 4];
        const auto r = nfs_layer_blend(pixels.data(), reinterpret_cast<const uint8_t*>(weights.data()), count, alphaMax, kThreshold, cw, 0x12345678u);
        CHECK(r.eax == model(pixels, weights, alphaMax), "count %d max %d: %08x, model %08x", count, alphaMax, r.eax, model(pixels, weights, alphaMax));
        ++blended;
#ifdef NFS_TEST_LIFTED_BLEND
        compareLifted(pixels, weights, alphaMax, cw);
#endif
    }
    // Odd weights: NaN and negative skip the layer; an infinite one converts as the model converts (the body's
    // _ftol2), the same in both.
    {
        const std::vector<uint8_t> pixels = {10, 20, 30, 40, 50, 60, 70, 80};
        const std::vector<float> odd[] = {{std::numeric_limits<float>::quiet_NaN(), 0.5f}, {-1.0f, 0.5f},
                                          {std::numeric_limits<float>::infinity(), 0.5f}, {0.5f, -std::numeric_limits<float>::infinity()}};
        for (const auto& weights : odd)
            for (int max = 0; max < 2; ++max) {
                const auto r = nfs_layer_blend(pixels.data(), reinterpret_cast<const uint8_t*>(weights.data()), 2, uint8_t(max), kThreshold, 0x027F, 0);
                if (!std::isinf(weights[0])) CHECK(r.eax == model(pixels, weights, max), "odd weight: %08x, model %08x", r.eax, model(pixels, weights, max));
#ifdef NFS_TEST_LIFTED_BLEND
                compareLifted(pixels, weights, max, 0x027F);
#endif
            }
    }
    // What the body leaves besides the pixel.
    {
        const std::vector<uint8_t> pixels = {1, 2, 3, 200, 10, 20, 30, 100, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        const std::vector<float> all = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
        auto run = [&](int count, std::vector<float> weights) {
            return nfs_layer_blend(pixels.data(), reinterpret_cast<const uint8_t*>(weights.data()), count, 0, kThreshold, 0x027F, 0xABCD0000u);
        };
        auto none = run(0, {});
        CHECK(none.eax == 0 && none.ecx == 0 && !none.edx_written && !none.arg0_written, "no layer: nothing but a zero pixel");
        auto two = run(2, {0.5f, 0.5f});
        CHECK(two.edx_written && two.edx == 0 && two.arg0_written && two.arg0 == 100, "two layers: edx the high dword of the last conversion, the slot its alpha byte");
        CHECK(two.ecx == 0 + 5 && (two.eax & 0xFF) == 5, "ecx the blue sum (1*0.5 -> 0, 10*0.5 -> 5)");
        auto skipped = run(3, {0.0f, 0.0f, 0.0f});
        CHECK(skipped.edx_written && skipped.edx == 0xABCD0000u && !skipped.arg0_written, "every layer skipped: edx holds the pixels pointer the loop loaded");
        auto four = run(4, {0.5f, 0.5f, 0.5f, 0.5f});
        CHECK(four.edx_written && four.edx == 0 && !four.arg0_written, "four layers at once: the slot untouched");
        auto five = run(5, all);
        CHECK(five.arg0_written && five.arg0 == 16, "five: the fifth layer goes one at a time, its alpha byte left in the slot");
        auto fourSkipped = run(4, {0.0f, 0.0f, 0.0f, 0.0f});
        CHECK(!fourSkipped.edx_written && !fourSkipped.arg0_written, "four skipped at once: edx untouched");
        auto lastSkipped = run(5, {0.5f, 0.5f, 0.5f, 0.5f, 0.0f});
        CHECK(lastSkipped.edx_written && lastSkipped.edx == 0xABCD0000u && !lastSkipped.arg0_written, "the fifth skipped: the loop's load stays in edx");
        std::vector<float> heavy = {2.0f, 2.0f};
        auto clamped = run(2, heavy);
        CHECK((clamped.eax >> 24) == 255 && ((clamped.eax >> 16) & 0xFF) == 66 && clamped.ecx == 2 + 20, "sums over 255 clamp, the others stay");
    }
    std::printf("layer_blend: %ld blends, %ld checks, %d failures\n", blended, checks, failures);
    return failures ? 1 : 0;
}
