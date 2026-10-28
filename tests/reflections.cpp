// Reflection target sizes (src/runtime/nfs_reflections.h): the originals, and the sharper ones derived from
// the render height and the picture's shape.
#include "../src/runtime/nfs_reflections.h"
#include <cstdio>
#include <cstdlib>
static void require(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
static void check(int hd, uint32_t w, uint32_t h, float aspect, nfs_reflection_sizes want, const char* what) {
    nfs_reflection_sizes got = nfs_refl_sizes(hd, w, h, aspect);
    if (got.road_w != want.road_w || got.road_h != want.road_h || got.cube != want.cube || got.mirror_w != want.mirror_w || got.mirror_h != want.mirror_h) {
        std::fprintf(stderr, "FAIL: %s: road %ux%u cube %u mirror %ux%u\n", what, got.road_w, got.road_h, got.cube, got.mirror_w, got.mirror_h);
        std::exit(1);
    }
}
int main() {
    const nfs_reflection_sizes original = { 320, 240, 128, 256, 64 };
    const float pixel = 2992.0f / 1344.0f;
    check(0, 2992, 1344, pixel, original, "off: the game's own sizes");
    check(1, 0, 0, pixel, original, "no render size: the game's own sizes");
    check(1, 2992, 1344, pixel, { 996, 448, 256, 896, 224 }, "native 2992x1344: a third of the height at 20:9, cube 256, mirror a sixth at 4:1");
    check(1, 2244, 1008, pixel, { 748, 336, 256, 672, 168 }, "75%");
    check(1, 1496, 672, pixel, { 534, 240, 256, 448, 112 }, "50%: the road never below 240 lines");
    check(1, 1792, 1344, 4.0f / 3.0f, { 596, 448, 256, 896, 224 }, "4:3 picture at the native height");
    check(1, 640, 300, 0.0f, { 512, 240, 256, 256, 64 }, "small: 240 lines at the picture's own shape (640/300), mirror and cube never below the originals");
    nfs_reflection_sizes s = nfs_refl_sizes(1, 2992, 1344, pixel);
    require(s.road_w % 2 == 0 && s.road_h % 2 == 0 && s.mirror_w % 2 == 0 && s.mirror_h % 2 == 0, "even sizes");
    require(NFS_REFL_CUBE_EDGE == 0x00702A84u && NFS_REFL_MIRROR_WIDTH == 0x00702A9Cu && NFS_REFL_MIRROR_HEIGHT == 0x00702AA0u,
            "the mod's data addresses in Speed.exe 1.4");
    std::puts("reflections: ok");
    return 0;
}
