#pragma once
// Pure policy for the launcher's graphics-quality options, kept free of D3D9/Android types so the
// host unit test (tests/graphics_quality.cpp) can exercise it.
//
//  * Texture filtering: DXVK 1.9.2 reads `d3d9.samplerAnisotropy` from the file named by
//    DXVK_CONFIG_FILE at Direct3DCreate9 time and applies it to every sampler the game creates.
//  * Anti-aliasing: Speed.exe never asks for multisampling. The bridge offers the requested sample
//    count to D3D9 when the device is created or reset, after the adapter confirmed it for the back
//    buffer and depth formats, falling back to the next lower count. MSAA requires SwapEffect DISCARD.
#include <string>

namespace graphics_quality {

inline unsigned clampAnisotropy(unsigned requested) {
    if (requested < 2) return 0;              // 0/1: leave the game's own filtering alone
    return requested > 16 ? 16u : requested;  // D3D9 caps top out at 16 on every Vulkan device DXVK drives
}

/** The dxvk.conf text for a filtering level; empty when nothing is to be forced. */
inline std::string dxvkConfig(unsigned anisotropy) {
    unsigned level = clampAnisotropy(anisotropy);
    std::string text = "# Written by the launcher before the game creates Direct3D; see graphics_quality.h\n";
    if (level) text += "d3d9.samplerAnisotropy = " + std::to_string(level) + "\n";
    return level ? text : std::string();
}

/**
 * The multisample count to use: the highest power of two <= requested (max 8) that `supported`
 * accepts, or 0. `supported(samples)` answers for both the colour and the depth format.
 */
template <class Supported>
unsigned chooseMultisample(unsigned requested, Supported supported) {
    unsigned level = requested >= 8 ? 8u : requested >= 4 ? 4u : requested >= 2 ? 2u : 0u;
    for (; level >= 2; level /= 2) if (supported(level)) return level;
    return 0;
}

}  // namespace graphics_quality
