#pragma once
// A direct-mapped cache in front of the D3D9 bridge's object table (d3d9_bridge.cpp).
//
// Every guest D3D9 call looks up its own COM wrapper, and many look up a resource argument as well
// (SetTexture, SetStreamSource, SetIndices, the shader setters): 25-90 thousand calls a race frame, each a
// hash lookup with a division and a node chase. Most calls name the device or the same few resources, so a
// small table indexed by the guest address answers them with one compare.
//
// unordered_map elements keep their address until they are erased (a rehash moves buckets, not nodes), so a
// slot can hold a pointer to the element. The contract with the owner of the map: forget(key) before erasing
// that key, forgetAll() after clearing the map. Guest heap blocks are 16-byte aligned, hence the shift.
// Lookups are serialised by the owner (the bridge runs under the guest machine lock); the hit and miss
// counts are atomics written that way and may be read from any thread.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

template <class Map, size_t Slots = 1024>
class ObjectLookup {
    static_assert((Slots & (Slots - 1)) == 0, "Slots must be a power of two");

public:
    using Value = typename Map::mapped_type;

    explicit ObjectLookup(Map& map) : map_(map) {}

    // The element for `key`, or nullptr when the map has none (a miss is not cached).
    Value* find(uint32_t key) {
        Slot& slot = slot_(key);
        if (slot.value && slot.key == key) { bump(hits_); return slot.value; }
        auto found = map_.find(key);
        if (found == map_.end()) return nullptr;
        bump(misses_);
        slot.key = key;
        slot.value = &found->second;
        return slot.value;
    }

    // Before the owner erases `key`.
    void forget(uint32_t key) {
        Slot& slot = slot_(key);
        if (slot.key == key) slot = Slot{};
    }

    // After the owner clears (or rebuilds) the map.
    void forgetAll() { slots_.fill(Slot{}); }

    uint64_t hits() const { return hits_.load(std::memory_order_relaxed); }
    uint64_t misses() const { return misses_.load(std::memory_order_relaxed); }
    void resetCounts() { hits_.store(0, std::memory_order_relaxed); misses_.store(0, std::memory_order_relaxed); }

private:
    struct Slot { uint32_t key = 0; Value* value = nullptr; };
    Slot& slot_(uint32_t key) { return slots_[(key >> 4) & (Slots - 1)]; }
    // One writer at a time: a plain load and store, no read-modify-write instruction.
    static void bump(std::atomic<uint64_t>& count) { count.store(count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }

    Map& map_;
    std::array<Slot, Slots> slots_{};
    std::atomic<uint64_t> hits_{0}, misses_{0};
};
