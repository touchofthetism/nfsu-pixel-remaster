// Guest path resolution with kept listings (android/app/src/main/cpp/guest_paths.h): the same host path as the
// runtime's uncached rule for every kind of name (found in another case, not there, through a folder that is not
// there, ".", a trailing separator, a symbolic link inside the folder and one leading out of it), the same entry
// when two differ only in case, what is kept and what changed() forgets, FindFirstFileA's listings, and many
// threads at once with changes among them.
#include "../android/app/src/main/cpp/guest_paths.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static int failures = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// The runtime's rule before build 20 (guest_files.inc gamePath), verbatim but for the exception: each component
// matched against a fresh listing of its folder, the result made canonical, refused when it leaves the root.
static std::string lowerRef(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
static bool reference(const fs::path& rootIn, const fs::path& relative, fs::path& out) {
    auto result = rootIn;
    for (const auto& part : relative) {
        if (part == ".") continue;
        std::error_code error; bool found = false;
        for (auto& entry : fs::directory_iterator(result, error))
            if (lowerRef(entry.path().filename().string()) == lowerRef(part.string())) { result = entry.path(); found = true; break; }
        if (!found) result /= part;
    }
    auto root = fs::weakly_canonical(rootIn); auto resolved = fs::weakly_canonical(result);
    auto check = resolved.lexically_relative(root);
    for (const auto& part : check) if (part == "..") return false;
    out = resolved;
    return true;
}

static void touch(const fs::path& path) { fs::create_directories(path.parent_path()); std::ofstream(path) << "x"; }

static std::vector<fs::path> samplePaths() {
    return {
        "", ".", "Tracks", "tracks", "TRACKS/", "tracks/streaml1ra.bun", "TRACKS/STREAML1RA.BUN", "Tracks/TrackB1001.LZC",
        "tracks/./trackb1001.lzc", "Tracks/TRACKB1000.LZC", "nosuch/dir/file.bin", "Global/globalb.lzc", "global",
        "nfs underground/SHEF.CFG", "NFS Underground", "cars/CAR1/textures.bin", "Cars/car1/", "dup/abc", "dup/ABC",
        "dup/Abc", "inside/streaml1ra.bun", "Inside/StreamL1RA.Bun", "outside", "outside/secret.txt", "Global/../Global",
    };
}

int main() {
    const fs::path base = fs::temp_directory_path() / ("guest_paths_" + std::to_string(::getpid()));
    fs::remove_all(base);
    const fs::path root = base / "nfsu1", away = base / "away";
    touch(root / "Tracks" / "STREAML1RA.BUN");
    touch(root / "Tracks" / "TrackB1001.lzc");
    touch(root / "Global" / "GlobalB.lzc");
    touch(root / "NFS Underground" / "shef.cfg");
    touch(root / "Cars" / "CAR1" / "TEXTURES.BIN");
    touch(root / "dup" / "abc");
    touch(root / "dup" / "ABC");      // the same name in another case: a case-sensitive folder holds both
    touch(away / "secret.txt");
    fs::create_directory_symlink(root / "Tracks", root / "inside");
    fs::create_directory_symlink(away, root / "outside");

    GuestPaths paths;
    paths.reset(root);
    CHECK(paths.canonicalRoot() == fs::weakly_canonical(root));

    // Every sample as the uncached rule resolves it, twice (the second from what was kept).
    for (int round = 0; round < 2; ++round)
        for (const auto& sample : samplePaths()) {
            const auto relative = sample.lexically_normal();
            bool hasDotDot = false;
            for (const auto& part : relative) if (part == "..") hasDotDot = true;
            if (hasDotDot) continue;   // the runtime refuses these before resolving
            fs::path expected, got;
            const bool expectedOk = reference(root, relative, expected), gotOk = paths.resolve(relative, got);
            CHECK(expectedOk == gotOk);
            if (expectedOk && gotOk && expected != got)
                std::fprintf(stderr, "  %s: expected %s, got %s\n", sample.string().c_str(), expected.string().c_str(), got.string().c_str());
            if (expectedOk && gotOk) CHECK(expected == got);
        }
    {
        fs::path got;
        CHECK(paths.resolve(fs::path("tracks/streaml1ra.bun"), got) && got == fs::weakly_canonical(root / "Tracks" / "STREAML1RA.BUN"));
        CHECK(!paths.resolve(fs::path("outside/secret.txt"), got));       // a link out of the folder is refused
        CHECK(paths.resolve(fs::path("Tracks/NewFile.bin"), got) && got == fs::weakly_canonical(root / "Tracks" / "NewFile.bin"));
    }
    auto stats = paths.stats();
    CHECK(stats.lookups > 0 && stats.cached > 0 && stats.cached < stats.lookups);
    const uint64_t listingsAfterFirst = stats.listings;

    // Kept: a file created behind its back is not seen until changed() (the runtime reports every change the
    // guest makes); the same lookups again read no folder.
    touch(root / "Tracks" / "NEWFILE.BIN");
    {
        fs::path got, expected;
        CHECK(paths.resolve(fs::path("Tracks/NewFile.bin"), got) && got == fs::weakly_canonical(root / "Tracks" / "NewFile.bin"));
        CHECK(paths.stats().listings == listingsAfterFirst);
        paths.changed();
        CHECK(paths.stats().changes == 1);
        CHECK(paths.resolve(fs::path("Tracks/NewFile.bin"), got));
        CHECK(reference(root, fs::path("Tracks/NewFile.bin"), expected) && got == expected);
        CHECK(got.filename() == "NEWFILE.BIN");
        CHECK(paths.stats().listings > listingsAfterFirst);
    }
    // A folder made and one removed, reported.
    {
        fs::create_directory(root / "Saves");
        paths.changed();
        fs::path got;
        CHECK(paths.resolve(fs::path("SAVES/a.sav"), got) && got == fs::weakly_canonical(root / "Saves" / "a.sav"));
        fs::remove(root / "Saves");
        paths.changed();
        CHECK(paths.resolve(fs::path("SAVES/a.sav"), got) && got == fs::weakly_canonical(root / "SAVES" / "a.sav"));
    }

    // FindFirstFileA's listings: the directory iterator's names and order; an error for a folder that is not there.
    {
        std::vector<std::string> names, expected;
        std::error_code error;
        CHECK(paths.listing(root / "Tracks", names, error) && !error);
        for (auto& entry : fs::directory_iterator(root / "Tracks")) expected.push_back(entry.path().filename().string());
        CHECK(names == expected);
        CHECK(!paths.listing(root / "NoSuchFolder", names, error) && error && names.empty());
        CHECK(error == std::errc::no_such_file_or_directory);
        const uint64_t before = paths.stats().listings;
        CHECK(paths.listing(root / "Tracks", names, error) && names == expected);
        CHECK(paths.stats().listings == before);   // kept
    }

    // A new game folder forgets the old one's.
    {
        const fs::path other = base / "other";
        touch(other / "TRACKS" / "x.bin");
        GuestPaths second;
        second.reset(root);
        fs::path got;
        CHECK(second.resolve(fs::path("tracks/x.bin"), got) && got == fs::weakly_canonical(root / "Tracks" / "x.bin"));
        second.reset(other);
        CHECK(second.resolve(fs::path("tracks/x.bin"), got) && got == fs::weakly_canonical(other / "TRACKS" / "x.bin"));
        CHECK(second.canonicalRoot() == fs::weakly_canonical(other));
    }

    // Many threads at once (the guest threads resolve under the machine lock, the boot workers do not), with
    // changes among them: every answer is the uncached rule's.
    {
        std::vector<std::pair<fs::path, fs::path>> expected;
        for (const auto& sample : samplePaths()) {
            const auto relative = sample.lexically_normal();
            bool skip = false;
            for (const auto& part : relative) if (part == "..") skip = true;
            fs::path out;
            if (!skip && reference(root, relative, out)) expected.emplace_back(relative, out);
        }
        std::atomic<int> wrong{0};
        std::atomic<bool> stop{false};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t)
            threads.emplace_back([&, t] {
                for (int i = 0; i < 2000; ++i) {
                    const auto& [relative, out] = expected[size_t(i * 7 + t) % expected.size()];
                    fs::path got;
                    if (!paths.resolve(relative, got) || got != out) wrong.fetch_add(1);
                }
            });
        std::thread changer([&] { while (!stop.load()) { paths.changed(); std::this_thread::yield(); } });
        for (auto& thread : threads) thread.join();
        stop = true;
        changer.join();
        CHECK(wrong.load() == 0);
    }

    fs::remove_all(base);
    std::printf("guest_paths: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
