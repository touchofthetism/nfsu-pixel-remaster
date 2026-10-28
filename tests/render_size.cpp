// The game's back buffer (src/runtime/nfs_widescreen.h, nfs_ws_back_buffer): the launcher's internal
// resolution replaces the game's 4:3 menu mode, which on its own stops at 1280x1024 (2280x1024 widened on a
// 2992x1344 phone, 1068x480 at the auto-detected 640x480).
#include "../src/runtime/nfs_widescreen.h"
#include <cstdio>
#include <cstdlib>
static void require(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
static void check(uint32_t gw, uint32_t gh, uint32_t rw, uint32_t rh, float aspect, uint32_t ew, uint32_t eh, const char* what) {
    uint32_t w = 0, h = 0;
    nfs_ws_back_buffer(gw, gh, rw, rh, aspect, &w, &h);
    if (w != ew || h != eh) { std::fprintf(stderr, "FAIL: %s: got %ux%u, want %ux%u\n", what, w, h, ew, eh); std::exit(1); }
}
int main() {
    const float pixel = 2992.0f / 1344.0f;   // Pixel 10 Pro XL, landscape
    // Widescreen on: every menu mode becomes the launcher's size at the panel's shape.
    check(1280, 1024, 2992, 1344, pixel, 2992, 1344, "Native from the game's 1280x1024");
    check(640, 480, 2992, 1344, pixel, 2992, 1344, "Native from the auto-detected 640x480");
    check(800, 600, 2992, 1344, pixel, 2992, 1344, "Native in the front end's 800x600");
    check(1280, 1024, 2244, 1008, pixel, 2244, 1008, "75%");
    check(1280, 1024, 1496, 672, pixel, 1496, 672, "50%");
    check(1280, 1024, 1602, 720, pixel, 1602, 720, "720 lines at 20:9");
    check(1280, 1024, 1281, 721, pixel, 1602, 720, "odd sizes are made even, width from the height");
    // Widescreen off: the launcher's height with a 4:3 width, stretched by the swap chain like the original.
    check(1280, 1024, 2992, 1344, 0.0f, 1792, 1344, "4:3 at the native height");
    check(640, 480, 1496, 672, 0.0f, 896, 672, "4:3 at 50%");
    // No launcher size (desktop hosts): the game's height stays, the width follows the display.
    check(1280, 1024, 0, 0, pixel, 2280, 1024, "the build-8 behaviour: 1280x1024 widened");
    check(640, 480, 0, 0, pixel, 1068, 480, "640x480 widened");
    check(1280, 1024, 0, 0, 0.0f, 1280, 1024, "no size, no fix: the game's own mode");
    check(1280, 1024, 200, 100, pixel, 2280, 1024, "a size below 320x240 is ignored");
    require(true, "");
    std::puts("render_size: ok");
    return 0;
}
