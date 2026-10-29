// The Garage decisions (src/runtime/nfs_garage.h): the branch override table against config/branches.txt and
// config/hooks.txt, each feature's forced outcomes, and the two register results the hooks leave.
#include "../src/runtime/nfs_garage.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
static void require(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }

// The VAs a config list names (first column, '#' comments), as scripts/run_lift.py reads them.
static std::set<uint32_t> configList(const char* name) {
    std::string path = __FILE__;
    path = path.substr(0, path.find_last_of('/')) + "/../config/" + name;
    std::ifstream file(path);
    require(bool(file), "config list readable");
    std::set<uint32_t> out;
    std::string line;
    while (std::getline(file, line)) {
        line = line.substr(0, line.find('#'));
        std::istringstream fields(line);
        std::string va;
        if (fields >> va) out.insert(uint32_t(std::stoul(va, nullptr, 16)));
    }
    return out;
}

int main() {
    // The table: sorted, unique, one known feature each, so the binary search and the counters hold.
    const unsigned count = NFS_BRANCH_OVERRIDE_COUNT;
    require(count == 20, "twenty overridden jumps");
    std::set<uint32_t> table;
    for (unsigned i = 0; i < count; ++i) {
        const nfs_branch_override& site = k_nfs_branch_overrides[i];
        if (i) require(k_nfs_branch_overrides[i - 1].va < site.va, "sorted by VA, no duplicates");
        require(site.feature && (site.feature & (site.feature - 1)) == 0 && (site.feature & NFS_GARAGE_ALL) == site.feature,
                "one feature bit each");
        require(site.feature != NFS_GARAGE_UNLOCK_ALL && site.feature != NFS_GARAGE_SPECIAL_VINYLS && site.feature != NFS_GARAGE_RACE_BRAKES,
                "unlock everything is a byte, special vinyls and race brakes are hooks");
        require(site.take == 0 || site.take == 1, "a jump is forced one way");
        require(site.va >= 0x00401000u && site.va < 0x00690000u, "inside Speed.exe's code");
        require(nfs_branch_override_index(site.va) == int(i), "found by its VA");
        table.insert(site.va);
    }
    require(nfs_branch_override_index(0x00400000u) == -1 && nfs_branch_override_index(0x0057EAF7u) == -1, "anything else is unknown");
    // The lifter wraps exactly the jumps the table decides.
    require(configList("branches.txt") == table, "config/branches.txt lists exactly the table's VAs");
    std::set<uint32_t> hooks = configList("hooks.txt");
    require(hooks.count(NFS_GARAGE_HOOK_HIDDEN_VINYL) && hooks.count(NFS_GARAGE_HOOK_BRAKE_LOD), "both hooks are in config/hooks.txt");
    for (uint32_t va : table) require(!hooks.count(va), "no site is both a hook and a branch");

    // Off, every jump keeps its lifted outcome; on, the feature's jumps take the table's way, the others not.
    for (unsigned i = 0; i < count; ++i) {
        const nfs_branch_override& site = k_nfs_branch_overrides[i];
        for (int taken = 0; taken <= 1; ++taken) {
            require(nfs_branch_decision(0, site.va, taken) == taken, "no feature: the game's outcome");
            require(nfs_branch_decision(NFS_GARAGE_ALL & ~site.feature, site.va, taken) == taken, "other features leave it alone");
            require(nfs_branch_decision(site.feature, site.va, taken) == site.take, "its feature forces it");
            require(nfs_branch_decision(NFS_GARAGE_ALL, site.va, taken) == site.take, "all on forces it");
        }
    }
    require(nfs_branch_decision(NFS_GARAGE_ALL, 0x00401000u, 1) == 1 && nfs_branch_decision(NFS_GARAGE_ALL, 0x00401000u, 0) == 0,
            "an unknown VA keeps its outcome");
    // The patches each feature stands for (sites read from the mods' code, see nfs_garage.h).
    auto forced = [&](uint32_t feature) {
        std::set<std::pair<uint32_t, int>> out;
        for (unsigned i = 0; i < count; ++i)
            if (k_nfs_branch_overrides[i].feature == feature) out.insert({k_nfs_branch_overrides[i].va, k_nfs_branch_overrides[i].take});
        return out;
    };
    require(forced(NFS_GARAGE_HOOD_VINYLS) == std::set<std::pair<uint32_t, int>>{
                {0x004E465Bu, 1}, {0x00508AD1u, 1}, {0x00508ADDu, 1}, {0x00508AE9u, 1}, {0x00508AF5u, 1}},
            "hood vinyls: the gate's STOCK jump and the four empty-layer jumps of the hood install");
    require(forced(NFS_GARAGE_VINYL_LAYERS) == std::set<std::pair<uint32_t, int>>{
                {0x004E46E4u, 1}, {0x004E46F2u, 1}, {0x004E46FEu, 1}, {0x004E470Au, 1},
                {0x004E4791u, 1}, {0x004E479Bu, 1}, {0x004E47A5u, 1}, {0x004E47AFu, 1}},
            "vinyl layers: Extra Options' No Restrictions, both halves");
    require(forced(NFS_GARAGE_HOOD_DECALS) == std::set<std::pair<uint32_t, int>>{
                {0x00508B86u, 1}, {0x00508F52u, 1}, {0x005104FEu, 1}, {0x0057EAF6u, 0}, {0x0057EB16u, 0}, {0x0057EBA6u, 0}},
            "hood decals: no question, no removal, the decal menu open, EXCLUDEDECAL never found");
    require(forced(NFS_GARAGE_ALL_DECALS) == std::set<std::pair<uint32_t, int>>{{0x0051222Au, 1}}, "all decals: one jump");

    // Hooks: special vinyls clear al (the hidden test's answer), race brakes clear cl (the LOD set); the rest stays.
    require(nfs_garage_hidden_vinyl_eax(NFS_GARAGE_SPECIAL_VINYLS, 0x12345601u) == 0x12345600u, "special vinyls: not hidden");
    require(nfs_garage_hidden_vinyl_eax(NFS_GARAGE_ALL & ~NFS_GARAGE_SPECIAL_VINYLS, 0x12345601u) == 0x12345601u, "off: the game's answer");
    require(nfs_garage_brake_lod_ecx(NFS_GARAGE_RACE_BRAKES, 1u) == 0u, "race brakes: the first, full-detail set");
    require(nfs_garage_brake_lod_ecx(0, 1u) == 1u && nfs_garage_brake_lod_ecx(NFS_GARAGE_RACE_BRAKES, 0u) == 0u, "off, or already 0: unchanged");
    require(NFS_GARAGE_HOOK_HIDDEN_VINYL == 0x004E27D8u && NFS_GARAGE_HOOK_BRAKE_LOD == 0x005702F8u, "the instructions after the patched ones");

    // The bits match the launcher (GameOptions.GARAGE_KEYS, same order) and the telemetry keys.
    const char* keys[] = {"unlock", "specialVinyls", "vinylLayers", "hoodVinyls", "hoodDecals", "allDecals", "raceBrakes"};
    require(sizeof keys / sizeof keys[0] == NFS_GARAGE_FEATURE_COUNT && NFS_GARAGE_ALL == (1u << NFS_GARAGE_FEATURE_COUNT) - 1,
            "seven features, all bits");
    for (unsigned i = 0; i < NFS_GARAGE_FEATURE_COUNT; ++i) {
        require(std::strcmp(nfs_garage_feature_key(i), keys[i]) == 0, "telemetry key in launcher order");
        require(std::strlen(nfs_garage_feature_name(i)) > 8, "a readable name for the log");
    }
    require(std::strcmp(nfs_garage_feature_key(NFS_GARAGE_FEATURE_COUNT), "?") == 0, "out of range is marked");
    require(NFS_GARAGE_UNLOCK_FLAG == 0x00735EC1u, "Extra Options' _UnlockEverythingThatCanBeLocked");
    std::puts("garage: ok");
    return 0;
}
