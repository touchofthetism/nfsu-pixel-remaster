#pragma once
// The render, texture stage and sampler states of Speed.exe's effects, applied by the host (build 17).
//
// The game draws through the D3DX effect framework linked into Speed.exe 1.4 (DirectX 9.0 SDK, 2003; the
// effects are fx/*.efx). Every pass of every draw applies all of its states again: CEffect::Pass (0x5D01EC)
// walks the pass's state records (0xAC bytes each), updates each value (0x5CDCA7) and applies it to the
// device (0x5C97CB). Build 16's telemetry: about 20,000 render, sampler and texture stage states a frame,
// of which the bridge's shadow found 96 % already set (1,080,522,289 of 1,123,276,754 in one session); the
// lifted D3DX code that got them there was 43 % of the render thread's game time (sub_005C97CB alone 17.5 %).
//
// Here those three kinds are decoded from the record and handed to the bridge, which drops the ones already
// set (d3d9EffectState): no lifted D3DX code and no guest call for them. The value update returns at once
// for a state whose value is fixed; that test runs here too. Everything else (lights, materials, transforms,
// shaders, textures, state arrays, values computed from parameters) keeps the game's own code.
//
// The record layout and the state table were read from the code (0x5C97CB-0x5CA0F8, 0x5CDCA7-0x5CDEB8):
//   +0x00 state index into the table at 0x6DC274 (0x1C bytes an entry, 179 entries), or -1: then +0x64
//         points at the record that holds it
//   +0x04 the stage or sampler, when the caller passes -1
//   +0x28 bit 0: the value is computed (from parameters or an expression), not fixed
//   +0x68 the value
// Each table entry starts with (category << 24 | id): category 1 a render state (id = D3DRENDERSTATETYPE,
// device method 57), 2 a texture stage state (67), 8 a sampler state (69); 3 lights, 4 materials, 5, 6
// shaders and textures, 7 transforms, 9 arrays, 10 the N-patch mode.
#include <cstdint>

namespace effect_states {

constexpr uint32_t kApply = 0x005C97CBu;      // thiscall (record, stage) -> HRESULT, ret 8
constexpr uint32_t kEvaluate = 0x005CDCA7u;   // thiscall (record) -> HRESULT, ret 4
constexpr uint32_t kStateTable = 0x006DC274u;
constexpr uint32_t kStateCount = 179;
constexpr uint32_t kStride = 0x1C;
constexpr uint32_t kIndirect = 0xFFFFFFFFu;

enum class Method : unsigned { None = 0, RenderState = 57, TextureStageState = 67, SamplerState = 69 };

struct Call {
    Method method = Method::None;
    uint32_t a = 0, b = 0, c = 0;   // the device method's arguments after the device
};

// The device call 0x5C97CB would make for (record, stage), or Method::None when it would do anything else.
template <class Read32>
Call applyCall(const Read32& read32, uint32_t record, uint32_t stage) {
    if (stage == kIndirect) stage = read32(record + 4);   // read before the record is followed, as the game does
    if (read32(record) == kIndirect) record = read32(record + 0x64);
    uint32_t index = read32(record);
    if (index >= kStateCount) return {};
    uint32_t type = read32(kStateTable + index * kStride);
    uint32_t id = type & 0x00FFFFFFu, value = read32(record + 0x68);
    switch (type >> 24) {
    case 1: return {Method::RenderState, id, value, 0};
    case 2: return {Method::TextureStageState, stage, id, value};
    case 8: return {Method::SamplerState, stage, id, value};
    default: return {};
    }
}

// What 0x5C97CB returns after the device call (its common exit, 0x5CA0D2): S_OK when the call succeeded or
// the effect ignores failures ([effect+84h] == 1), the HRESULT otherwise.
inline uint32_t applyResult(uint32_t hresult, uint32_t ignoreFailures) {
    return (static_cast<int32_t>(hresult) >= 0 || ignoreFailures == 1) ? 0u : hresult;
}

// True when 0x5CDCA7 would return S_OK at once (0x5CDEAE test byte [record+28h], 1 / je 0x5CE172) without
// writing anything: a fixed value, and not the vertex or pixel shader state (06000000h, 06000001h), whose
// update takes the long path first. An index outside the table is left to the game.
template <class Read32, class Read8>
bool evaluateIsNoop(const Read32& read32, const Read8& read8, uint32_t record) {
    uint32_t index = read32(record);
    if (index != kIndirect) {
        if (index >= kStateCount) return false;
        uint32_t type = read32(kStateTable + index * kStride);
        if (type == 0x06000000u || type == 0x06000001u) return false;
    }
    return (read8(record + 0x28) & 1u) == 0;
}

}  // namespace effect_states
