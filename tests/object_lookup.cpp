// The D3D9 bridge's object lookup cache (android/app/src/main/cpp/object_lookup.h): hits return the map's own
// element, misses are not cached, two addresses sharing a slot both stay correct, a forgotten and reused
// address finds the new element, and cached pointers survive the map growing (rehashing) under them.
#include "../android/app/src/main/cpp/object_lookup.h"
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
static void require(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
struct Entry { int kind; unsigned references; };
using Table = std::unordered_map<uint32_t, Entry>;
int main() {
    Table objects;
    ObjectLookup<Table> lookups(objects);
    const uint32_t device = 0x00A10010u;
    objects.emplace(device, Entry{2, 1});
    require(lookups.find(device) == &objects.at(device), "the first lookup returns the map's element");
    require(lookups.misses() == 1 && lookups.hits() == 0, "the first lookup went to the table");
    for (int i = 0; i < 100; ++i) require(lookups.find(device) == &objects.at(device), "repeat lookups return the element");
    require(lookups.hits() == 100 && lookups.misses() == 1, "repeat lookups are answered by the cache");
    // Writes through the cached pointer are writes to the map.
    ++lookups.find(device)->references;
    require(objects.at(device).references == 2, "the cached pointer is the element itself");

    // Unknown addresses: nullptr, and nothing is cached for them.
    require(lookups.find(0x00B00000u) == nullptr, "an unknown address");
    require(lookups.find(0x00B00000u) == nullptr, "still unknown");
    objects.emplace(0x00B00000u, Entry{4, 1});
    require(lookups.find(0x00B00000u) == &objects.at(0x00B00000u), "an address added after a miss is found");

    // Two addresses 16 KB apart share a slot (1024 slots of 16 bytes): each lookup still returns its own.
    const uint32_t a = 0x00C00000u, b = a + 1024u * 16u;
    objects.emplace(a, Entry{5, 1});
    objects.emplace(b, Entry{6, 1});
    for (int i = 0; i < 10; ++i) {
        require(lookups.find(a)->kind == 5, "slot shared: a");
        require(lookups.find(b)->kind == 6, "slot shared: b");
    }

    // Retire and reuse an address (the guest heap hands freed blocks out again): forget before erase.
    const uint32_t texture = 0x00D00020u;
    objects.emplace(texture, Entry{4, 1});
    require(lookups.find(texture)->kind == 4, "the texture");
    lookups.forget(texture);
    objects.erase(texture);
    require(lookups.find(texture) == nullptr, "a retired address is gone");
    objects.emplace(texture, Entry{5, 7});
    require(lookups.find(texture) == &objects.at(texture) && lookups.find(texture)->references == 7, "a reused address finds the new object");
    // forget() of an address that does not own its slot leaves the owner cached.
    require(lookups.find(a)->kind == 5, "a owns its slot");
    uint64_t hits = lookups.hits();
    lookups.forget(b);
    require(lookups.find(a)->kind == 5 && lookups.hits() == hits + 1, "forgetting b keeps a cached");

    // Growing the map rehashes it; nodes do not move, so cached pointers stay valid.
    Entry* cached = lookups.find(device);
    for (uint32_t i = 0; i < 20000; ++i) objects.emplace(0x01000000u + i * 16u, Entry{4, 1});
    require(cached == &objects.at(device), "elements keep their address across a rehash");
    require(lookups.find(device) == cached && lookups.find(device)->references == 2, "the cached element after a rehash");
    for (uint32_t i = 0; i < 20000; i += 997) require(lookups.find(0x01000000u + i * 16u) == &objects.at(0x01000000u + i * 16u), "lookups after growth");

    // Shutdown: the map is cleared, then the cache.
    objects.clear();
    lookups.forgetAll();
    require(lookups.find(device) == nullptr && lookups.find(a) == nullptr, "nothing after a shutdown");
    objects.emplace(device, Entry{2, 1});
    require(lookups.find(device) == &objects.at(device), "a new device after a restart");
    lookups.resetCounts();
    require(lookups.hits() == 0 && lookups.misses() == 0, "counts reset");
    std::puts("object lookup ok");
    return 0;
}
