// The sampling profile of the guest machine (android/app/src/main/cpp/guest_profiler.h): totals per place,
// the render thread's functions, other threads, idle, the slow-frame explanation and the JSON record.
#include "../android/app/src/main/cpp/guest_profiler.h"
#include <cstdio>
#include <cstdlib>
#include <string>
static void require(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
static bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
int main() {
    GuestProfile p;
    uint64_t t = 1000000000ull;
    auto add = [&](GuestProfile::Holder h, uint32_t f, GuestProfile::Place place, int n) {
        for (int i = 0; i < n; ++i) { p.add(t, h, f, place); t += 1000000; }   // 1 ms apart
    };
    // A 100 ms window: 50 ms in one lifted function, 30 ms in D3D9 calls made by another, 10 ms of the sound
    // thread, 10 ms with nobody holding the machine.
    uint64_t from = t;
    add(GuestProfile::Render, 0x004A1B20u, GuestProfile::Guest, 50);
    add(GuestProfile::Render, 0x0040B9C0u, GuestProfile::D3D9, 30);
    add(GuestProfile::Other, 0x0052D0C0u, GuestProfile::Guest, 10);
    add(GuestProfile::Nobody, 0, GuestProfile::Guest, 10);
    uint64_t to = t;
    std::string why = p.explain(from, to);
    require(has(why, "sub_004A1B20 guest 50%, sub_0040B9C0 D3D9 30%"), "top pairs first, with their place");
    require(has(why, "other threads 10%, idle 10% (100 samples)"), "other threads and idle shares");
    require(p.explain(to + 1, to + 2) == "no samples", "an empty window");
    // Outside the window: not counted in the explanation, counted in the totals.
    add(GuestProfile::Render, 0x00585B40u, GuestProfile::Import, 5);
    require(!has(p.explain(from, to), "00585B40"), "the window bounds the explanation");
    std::string json = p.json(1000, 2, 1);
    require(has(json, "\"intervalUs\":1000,\"samples\":105,"), "sample count");
    require(has(json, "\"render\":{\"guest\":50,\"d3d9\":30,\"import\":5},\"other\":10,\"idle\":10"), "totals per place");
    require(has(json, "\"top\":[[\"004A1B20\",50,0,0],[\"0040B9C0\",0,30,0]]"), "the render thread's top functions, capped");
    require(has(json, "\"otherTop\":[[\"0052D0C0\",10]]"), "other threads' functions");
    int depth = 0; for (char c : json) { if (c == '{' || c == '[') ++depth; if (c == '}' || c == ']') --depth; require(depth >= 0, "nesting"); }
    require(depth == 0, "balanced json");
    // The ring keeps the last kRing samples only.
    GuestProfile ring;
    for (size_t i = 0; i < GuestProfile::kRing + 10; ++i) ring.add(i, GuestProfile::Render, 1, GuestProfile::Guest);
    require(has(ring.explain(0, GuestProfile::kRing + 20), "(1024 samples)"), "explanations come from the ring");
    require(ring.samples() == GuestProfile::kRing + 10, "totals keep every sample");
    std::puts("guest_profiler: ok");
    return 0;
}
