#pragma once
// Guest file names to host paths, case-insensitively as Windows finds them, with what the lookups found kept.
//
// The guest names files below its folder C:\nfsu1 in any case (TRACKS\STREAML1RA.BUN, Tracks\StreamL1RA.bun);
// the host folder may be case-sensitive (the test host's ext4). Each component of the path matches the folder
// entry that equals it ignoring ASCII case, the first in the folder's listing order; a component no entry
// matches is used as written (a file about to be created, or one the host serves in its place). The result is
// made canonical (symbolic links followed) and must stay inside the game folder.
//
// Why kept: the phone's game folder is on Android's shared storage, a FUSE file system that MediaProvider serves,
// so every folder listing and every attribute read is a round trip to its daemon (about 1-2 ms a listing on the
// Pixel 10 Pro XL). Resolving a name listed each folder on its path and canonicalised the result twice, and the
// game's file layer names each file several times before it reads it (the full path, the drive letter, the real
// name, then the open). Build 19's 76-minute session spent 28 s of the guest machine's time there - 17 s in
// GetFullPathNameA alone - mostly in loading screens and the car lot, with the render thread waiting for the
// machine: it was the largest single cost of a race load after the game's own work.
//
// Kept until something changes the folder: the guest creating, deleting or renaming a file or folder (the
// runtime reports each through changed()), or the app coming back from the background (the launcher may have
// installed or removed a mod pack meanwhile). Saves and options rewrite existing files, which changes no
// listing. Nothing else writes the game folder while the game runs.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

class GuestPaths {
public:
    struct Stats {
        uint64_t lookups = 0;      // resolve() calls
        uint64_t cached = 0;       // ... answered from what was kept
        uint64_t listings = 0;     // folder listings read from the file system
        uint64_t changes = 0;      // changed(): everything forgotten
        uint64_t resolveNs = 0;    // time the lookups not kept took (listings, canonicalisation)
    };

    // ASCII lower case, as the runtime's lower() (std::tolower in the "C" locale).
    static std::string lower(std::string text) {
        for (auto& c : text) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        return text;
    }

    // A new game folder: forget everything.
    void reset(std::filesystem::path root) {
        std::lock_guard<std::mutex> lock(mutex_);
        root_ = std::move(root);
        haveCanonicalRoot_ = false;
        clearLocked();
    }

    // The game folder made canonical (weakly), computed once per folder.
    std::filesystem::path canonicalRoot() {
        std::lock_guard<std::mutex> lock(mutex_);
        return canonicalRootLocked();
    }

    // root / relative (lexically normal, no ".." component: the caller has refused those) resolved as described
    // above, into `out`. False when the canonical result leaves the game folder (a symbolic link pointing out).
    bool resolve(const std::filesystem::path& relative, std::filesystem::path& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.lookups;
        std::string key = relative.generic_string();
        auto kept = resolved_.find(key);
        if (kept != resolved_.end()) {
            ++stats_.cached;
            out = kept->second;
            return true;
        }
        const auto start = std::chrono::steady_clock::now();
        std::filesystem::path result = root_;
        for (const auto& part : relative) {
            if (part == ".") continue;
            const Listing& listing = listingLocked(result);
            auto match = listing.byLower.find(lower(part.string()));
            if (match != listing.byLower.end()) result /= listing.names[match->second];
            else result /= part;
        }
        const std::filesystem::path root = canonicalRootLocked();
        std::filesystem::path resolved = std::filesystem::weakly_canonical(result);
        stats_.resolveNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        for (const auto& part : resolved.lexically_relative(root)) if (part == "..") return false;
        resolved_.emplace(std::move(key), resolved);
        out = std::move(resolved);
        return true;
    }

    // The entries of a host folder in listing order ("." and ".." left out, as std::filesystem lists), for
    // FindFirstFileA. False, with the error the listing gave, when the folder cannot be listed.
    bool listing(const std::filesystem::path& directory, std::vector<std::string>& names, std::error_code& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        const Listing& listing = listingLocked(directory);
        names = listing.names;
        error = listing.error;
        return !listing.error;
    }

    // The guest created, deleted or renamed something in the game folder, or the app came back from the
    // background: forget every listing and resolution (the next lookups read the folders again).
    void changed() {
        std::lock_guard<std::mutex> lock(mutex_);
        clearLocked();
        ++stats_.changes;
    }

    Stats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

private:
    struct Listing {
        std::vector<std::string> names;                      // listing order
        std::unordered_map<std::string, size_t> byLower;     // lower-case name -> first entry with it
        std::error_code error;
    };

    const Listing& listingLocked(const std::filesystem::path& directory) {
        std::string key = directory.string();
        auto kept = listings_.find(key);
        if (kept != listings_.end()) return *kept->second;
        auto listing = std::make_unique<Listing>();
        std::error_code error;
        for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
            std::string name = it->path().filename().string();
            listing->byLower.emplace(lower(name), listing->names.size());   // emplace keeps the first
            listing->names.push_back(std::move(name));
        }
        listing->error = error;
        ++stats_.listings;
        return *listings_.emplace(std::move(key), std::move(listing)).first->second;
    }

    const std::filesystem::path& canonicalRootLocked() {
        if (!haveCanonicalRoot_) {
            canonicalRoot_ = std::filesystem::weakly_canonical(root_);
            haveCanonicalRoot_ = true;
        }
        return canonicalRoot_;
    }

    void clearLocked() {
        listings_.clear();
        resolved_.clear();
    }

    mutable std::mutex mutex_;
    std::filesystem::path root_, canonicalRoot_;
    bool haveCanonicalRoot_ = false;
    std::unordered_map<std::string, std::unique_ptr<Listing>> listings_;
    std::unordered_map<std::string, std::filesystem::path> resolved_;
    Stats stats_;
};
