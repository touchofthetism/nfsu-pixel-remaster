// The effect state decoding of android/app/src/main/cpp/effect_states.h against a fake guest memory laid out as
// Speed.exe's D3DX effect leaves it: the state table at 0x6DC274 and records of 0xAC bytes.
#include "../android/app/src/main/cpp/effect_states.h"
#include <cstdio>
#include <cstdlib>
#include <map>
static void require(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }

struct Memory {
    std::map<uint32_t, uint8_t> bytes;
    void put32(uint32_t va, uint32_t value) { for (int i = 0; i < 4; ++i) bytes[va + i] = uint8_t(value >> (8 * i)); }
    void put8(uint32_t va, uint8_t value) { bytes[va] = value; }
    uint32_t get32(uint32_t va) const {
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) { auto it = bytes.find(va + i); value |= uint32_t(it == bytes.end() ? 0xCD : it->second) << (8 * i); }
        return value;
    }
    uint8_t get8(uint32_t va) const { auto it = bytes.find(va); return it == bytes.end() ? 0xCD : it->second; }
};

using effect_states::Method;
using effect_states::kStateTable;
using effect_states::kStride;

int main() {
    Memory memory;
    auto read32 = [&](uint32_t va) { return memory.get32(va); };
    auto read8 = [&](uint32_t va) { return memory.get8(va); };
    // Table entries as the game's: index 0 ZENABLE (render state 7), 1 a texture stage state (COLOROP 1),
    // 2 a sampler state (MAGFILTER 5), 3 a light field, 4 the vertex shader, 5 the pixel shader, 6 a transform.
    memory.put32(kStateTable + 0 * kStride, 0x01000007u);
    memory.put32(kStateTable + 1 * kStride, 0x02000001u);
    memory.put32(kStateTable + 2 * kStride, 0x08000005u);
    memory.put32(kStateTable + 3 * kStride, 0x03000002u);
    memory.put32(kStateTable + 4 * kStride, 0x06000000u);
    memory.put32(kStateTable + 5 * kStride, 0x06000001u);
    memory.put32(kStateTable + 6 * kStride, 0x07000001u);
    auto record = [&](uint32_t va, uint32_t index, uint32_t stage, uint32_t value, uint8_t flags) {
        memory.put32(va, index); memory.put32(va + 4, stage); memory.put32(va + 0x68, value); memory.put8(va + 0x28, flags);
    };
    record(0x01000000u, 0, 0, 1, 0);          // ZENABLE = TRUE, fixed
    record(0x01000100u, 1, 3, 4, 0);          // stage 3 COLOROP = MODULATE
    record(0x01000200u, 2, 2, 2, 1);          // sampler 2 MAGFILTER = LINEAR, value computed
    record(0x01000300u, 3, 0, 0x3F800000u, 0); // a light field
    record(0x01000400u, 4, 0, 0x02000000u, 0); // the vertex shader
    record(0x01000500u, 5, 0, 0x02000100u, 0); // the pixel shader
    record(0x01000600u, 6, 0, 0, 0);          // a transform

    auto call = effect_states::applyCall(read32, 0x01000000u, 0xFFFFFFFFu);
    require(call.method == Method::RenderState && call.a == 7 && call.b == 1, "render state: (state, value)");
    call = effect_states::applyCall(read32, 0x01000100u, 0xFFFFFFFFu);
    require(call.method == Method::TextureStageState && call.a == 3 && call.b == 1 && call.c == 4, "stage from the record when -1 is passed");
    call = effect_states::applyCall(read32, 0x01000100u, 5);
    require(call.method == Method::TextureStageState && call.a == 5, "a stage passed in is used as is");
    call = effect_states::applyCall(read32, 0x01000200u, 0xFFFFFFFFu);
    require(call.method == Method::SamplerState && call.a == 2 && call.b == 5 && call.c == 2, "sampler state: (sampler, type, value)");
    for (uint32_t other : {0x01000300u, 0x01000400u, 0x01000500u, 0x01000600u})
        require(effect_states::applyCall(read32, other, 0xFFFFFFFFu).method == Method::None, "lights, shaders, transforms: the game's code");

    // An indirect record (index -1) holds its state in the record at +0x64; the stage is the first record's.
    memory.put32(0x01000700u, 0xFFFFFFFFu); memory.put32(0x01000704u, 6); memory.put32(0x01000764u, 0x01000200u);
    call = effect_states::applyCall(read32, 0x01000700u, 0xFFFFFFFFu);
    require(call.method == Method::SamplerState && call.a == 6 && call.b == 5 && call.c == 2, "indirect: the target's state and value, the first record's stage");
    // Out of the table: left to the game.
    record(0x01000800u, effect_states::kStateCount, 0, 0, 0);
    require(effect_states::applyCall(read32, 0x01000800u, 0).method == Method::None, "index past the table");
    memory.put32(0x01000900u, 0xFFFFFFFFu); memory.put32(0x01000964u, 0x01000800u);
    require(effect_states::applyCall(read32, 0x01000900u, 0).method == Method::None, "indirect to an index past the table");

    // The result the apply returns after the device call.
    require(effect_states::applyResult(0, 0) == 0 && effect_states::applyResult(1, 0) == 0, "success: S_OK");
    require(effect_states::applyResult(0x8876086Cu, 0) == 0x8876086Cu, "a failure is returned");
    require(effect_states::applyResult(0x8876086Cu, 1) == 0, "unless the effect ignores failures ([effect+84h] == 1)");
    require(effect_states::applyResult(0x8876086Cu, 2) == 0x8876086Cu, "only the value 1 ignores them");

    // The value update's quick exit: a fixed value that is not a shader state.
    require(effect_states::evaluateIsNoop(read32, read8, 0x01000000u), "fixed render state: S_OK at once");
    require(effect_states::evaluateIsNoop(read32, read8, 0x01000100u), "fixed stage state: S_OK at once");
    require(!effect_states::evaluateIsNoop(read32, read8, 0x01000200u), "a computed value runs the game's update");
    require(!effect_states::evaluateIsNoop(read32, read8, 0x01000400u) && !effect_states::evaluateIsNoop(read32, read8, 0x01000500u),
            "the shader states take the long path first");
    require(effect_states::evaluateIsNoop(read32, read8, 0x01000600u), "a fixed transform: S_OK at once");
    memory.put8(0x01000728u, 0);
    require(effect_states::evaluateIsNoop(read32, read8, 0x01000700u), "indirect: only its own flag counts");
    memory.put8(0x01000728u, 1);
    require(!effect_states::evaluateIsNoop(read32, read8, 0x01000700u), "indirect, computed: the game's update");
    require(!effect_states::evaluateIsNoop(read32, read8, 0x01000800u), "index past the table: the game's code");
    std::puts("effect_states: ok");
    return 0;
}
