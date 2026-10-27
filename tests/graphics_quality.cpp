#include "../android/app/src/main/cpp/graphics_quality.h"
#include <cstdio>
#include <cstdlib>
#include <set>
using namespace graphics_quality;
static void require(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
int main() {
    require(clampAnisotropy(0) == 0 && clampAnisotropy(1) == 0, "0 and 1 mean the game's own filtering");
    require(clampAnisotropy(8) == 8 && clampAnisotropy(16) == 16, "8x and 16x pass through");
    require(clampAnisotropy(64) == 16, "anything above 16 is capped at the D3D9 maximum");
    require(dxvkConfig(0).empty(), "no filtering means no config file");
    require(dxvkConfig(16).find("d3d9.samplerAnisotropy = 16\n") != std::string::npos, "16x is written as DXVK expects");
    require(dxvkConfig(8).find("d3d9.samplerAnisotropy = 8\n") != std::string::npos, "8x is written as DXVK expects");

    auto any = [](unsigned) { return true; };
    auto none = [](unsigned) { return false; };
    require(chooseMultisample(0, any) == 0 && chooseMultisample(1, any) == 0, "off stays off");
    require(chooseMultisample(2, any) == 2 && chooseMultisample(4, any) == 4, "supported counts are used as asked");
    require(chooseMultisample(3, any) == 2, "non-power-of-two rounds down");
    require(chooseMultisample(16, any) == 8, "requests above 8 use 8");
    require(chooseMultisample(4, none) == 0, "nothing supported means no MSAA");
    std::set<unsigned> only2 = {2};
    require(chooseMultisample(4, [&](unsigned s) { return only2.count(s) != 0; }) == 2, "4x falls back to 2x when only 2x is supported");
    std::set<unsigned> only4 = {4};
    require(chooseMultisample(8, [&](unsigned s) { return only4.count(s) != 0; }) == 4, "8x falls back to 4x");
    unsigned asked = 0;
    chooseMultisample(8, [&](unsigned s) { asked |= s; return false; });
    require(asked == (8 | 4 | 2), "every lower count is tried before giving up");
    std::puts("Graphics quality policy passed: anisotropy clamp/config and MSAA fallback order");
}
