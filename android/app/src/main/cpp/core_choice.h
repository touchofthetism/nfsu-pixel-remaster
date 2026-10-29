#pragma once
// Which cpu cluster the game's render thread runs on (host_perf.cpp; tested in tests/core_choice.cpp).
//
// The game is one busy thread, and builds 3-11 pinned it to the biggest core (cpu7, the X4 on the Pixel 10 Pro
// XL), which is fastest only while the phone lets it run fast. It rarely does for long: in the long sessions of
// 2026-10-28 the X4 ran at 0.9-1.4 GHz for 60-84 % of the time the game drew frames, at any battery level, while
// the A725 cores carrying DXVK's threads averaged 2.1-2.5 GHz. The game followed the X4: 83-98 FPS in the
// seconds it ran at 2.6 GHz or more, 46-57 at 1.4 or less, with the GPU idle a quarter to half of the time.
//
// A cluster's speed for the thread is its capacity (the kernel's cpu_capacity: work per clock times top clock,
// 1024 for the biggest core) scaled by the clock it is allowed now (scaling_max_freq, the cap thermal and
// battery limits set, over cpuinfo_max_freq). The thread goes where that is highest, with hysteresis so it does
// not shuttle: another cluster must be 20 % faster for three checks in a row (one check a second) and the last
// move at least ten checks ago; the home cluster (the biggest) takes it back once it is as fast again for five
// checks. Clusters of less than half the home cluster's capacity (the little cores) are never chosen. A cap that
// cannot be read counts as the top clock, so a phone that hides its caps keeps build 11's placement. DXVK's
// threads move with it (helperCpus).
//
// Build 15: the kernel's capacities miss how much more the biggest core does per clock for this game. The Pixel
// 10 Pro XL's give the X4 and the A725 the same capacity per clock (1024 at 3.78 GHz, 824 at 3.05), and build 14
// moved the thread on that. Over the 10 s on each side of 25 of its moves (three sessions, cap 120 or none), the
// frame rate per MHz the thread's cluster ran at was 1.51 times as high on the X4 (the median; quartiles
// 1.29-1.71; seconds held by the frame cap or the GPU left out, and with them the 60 FPS session's four moves).
// 9 of the 14 moves away from the X4 lowered the frame rate (98.7 to 59.3 FPS, 56.9 to 35.4) and 9 of the 11 back
// raised it. So the home cluster's speed counts its work per clock as kHomeWorkPerClock times the best eligible
// cluster's wherever the kernel's capacities say less; another cluster then needs 1.8 times the home cluster's
// allowed clock to take the thread. Replayed through the recorded caps (2 s samples), build 14's three longest
// sessions stay on the X4 throughout, where the old rule moves the thread 13, 12 and 5 times (the sessions moved
// it 13, 11 and 4 times); only the deepest caps of builds 13-14 (the X4 allowed 1.15-1.4 GHz) still move it.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class CoreChoice {
public:
    struct Cluster {
        std::vector<int> cpus;     // the cpus sharing one clock (cpufreq policy)
        unsigned capacity = 0;     // cpu_capacity of its cpus (1024 = the biggest core at its top clock)
        unsigned maxKhz = 0;       // cpuinfo_max_freq
    };

    // Thresholds (see above).
    static constexpr double kLeaveFactor = 1.20;    // another cluster this much faster ...
    static constexpr unsigned kLeaveChecks = 3;      // ... for this many checks in a row
    static constexpr double kReturnFactor = 1.00;   // the home cluster at least as fast ...
    static constexpr unsigned kReturnChecks = 5;     // ... for this many checks in a row
    static constexpr unsigned kDwellChecks = 10;     // checks between two moves at least
    static constexpr double kEligible = 0.5;         // a cluster's capacity against the home cluster's
    static constexpr double kHomeWorkPerClock = 1.5; // the home cluster's work per clock against the others', at least

    CoreChoice() = default;
    explicit CoreChoice(std::vector<Cluster> clusters) : clusters_(std::move(clusters)) {
        for (size_t i = 0; i < clusters_.size(); ++i) {
            const Cluster& c = clusters_[i];
            const Cluster* h = home_ < 0 ? nullptr : &clusters_[size_t(home_)];
            if (!h || c.capacity > h->capacity || (c.capacity == h->capacity && c.maxKhz > h->maxKhz)) home_ = int(i);
        }
        current_ = home_;
        sinceMove_ = kDwellChecks;
        // The home cluster's per-clock advantage the kernel's capacities leave out (see above).
        if (home_ >= 0) {
            const Cluster& h = clusters_[size_t(home_)];
            double best = 0;
            for (size_t i = 0; i < clusters_.size(); ++i)
                if (int(i) != home_ && eligible(i) && clusters_[i].maxKhz)
                    best = std::max(best, double(clusters_[i].capacity) / clusters_[i].maxKhz);
            if (best > 0 && h.capacity && h.maxKhz)
                homeFactor_ = std::max(1.0, kHomeWorkPerClock / (double(h.capacity) / h.maxKhz / best));
        }
    }

    size_t size() const { return clusters_.size(); }
    const Cluster& cluster(size_t i) const { return clusters_[i]; }
    int home() const { return home_; }
    int current() const { return current_; }
    unsigned moves() const { return moves_; }
    // What the home cluster's capacity is multiplied by in speed(): 1 where the kernel already credits it with
    // kHomeWorkPerClock times the others' work per clock or more.
    double homeFactor() const { return homeFactor_; }

    bool eligible(size_t i) const {
        return home_ >= 0 && clusters_[i].capacity >= kEligible * clusters_[size_t(home_)].capacity;
    }

    // The cluster's speed with the given cap (kHz; 0 or above the top clock: the top clock).
    double speed(size_t i, unsigned capKhz) const {
        const Cluster& c = clusters_[i];
        double factor = int(i) == home_ ? homeFactor_ : 1.0;
        if (!c.maxKhz) return c.capacity * factor;
        unsigned khz = capKhz && capKhz < c.maxKhz ? capKhz : c.maxKhz;
        return double(c.capacity) * khz / c.maxKhz * factor;
    }

    // One check with each cluster's cap (scaling_max_freq, kHz, in cluster order; 0 = unknown). Returns true
    // when the thread should move now (current() is then the new cluster).
    bool check(const std::vector<unsigned>& capKhz) {
        if (home_ < 0 || capKhz.size() != clusters_.size()) return false;
        ++sinceMove_;
        // The fastest eligible cluster; ties go to the home cluster, then to the higher cpus (the second mid
        // cluster on Tensor, away from the first, where Android's own work tends to land).
        int best = home_;
        double bestSpeed = speed(size_t(home_), capKhz[size_t(home_)]);
        for (size_t k = clusters_.size(); k-- > 0;) {
            if (int(k) == home_ || !eligible(k)) continue;
            double s = speed(k, capKhz[k]);
            if (s > bestSpeed) { best = int(k); bestSpeed = s; }
        }
        double now = speed(size_t(current_), capKhz[size_t(current_)]);
        int target = -1;
        unsigned needed = 0;
        if (best != current_) {
            if (best == home_ && bestSpeed >= kReturnFactor * now) {
                target = best; needed = kReturnChecks;                  // home is as fast again
            } else if (best != home_ && bestSpeed >= kLeaveFactor * now) {
                target = best; needed = kLeaveChecks;                   // another cluster is clearly faster
            }
        }
        if (target < 0) { candidate_ = -1; streak_ = 0; return false; }
        if (target != candidate_) { candidate_ = target; streak_ = 0; }
        if (++streak_ < needed || sinceMove_ < kDwellChecks) return false;
        current_ = target;
        candidate_ = -1; streak_ = 0; sinceMove_ = 0; ++moves_;
        return true;
    }

    // The cpus for DXVK's threads while the render thread is on `cluster`: every other cluster's (they must not
    // take turns with it), and not the home cluster's either while the render thread has left it for being slow,
    // as long as another cluster of the eligible size remains for them. The command stream is on every frame's
    // critical path too: at the capped clock that made the render thread leave, the home cluster would hold the
    // frame back, and left idle it cools sooner. On a phone with one mid cluster that would leave only the
    // little cores, so there the home cluster stays allowed.
    std::vector<int> helperCpus(int cluster) const {
        std::vector<int> cpus;
        if (cluster < 0 || size_t(cluster) >= clusters_.size()) return cpus;
        bool skipHome = false;
        if (cluster != home_)
            for (size_t i = 0; i < clusters_.size(); ++i)
                if (int(i) != cluster && int(i) != home_ && eligible(i)) skipHome = true;
        for (size_t i = 0; i < clusters_.size(); ++i)
            if (int(i) != cluster && !(skipHome && int(i) == home_))
                cpus.insert(cpus.end(), clusters_[i].cpus.begin(), clusters_[i].cpus.end());
        std::sort(cpus.begin(), cpus.end());
        return cpus;
    }

    // A move check() asked for could not be made (the kernel refused the mask): back to `cluster`.
    void undo(int cluster) {
        if (cluster < 0 || size_t(cluster) >= clusters_.size()) return;
        current_ = cluster;
        if (moves_) --moves_;
    }

    // "cpu 7", "cpu 5-6", "cpu 0,2" for logs.
    static std::string describe(const std::vector<int>& cpus) {
        std::string out;
        for (size_t i = 0; i < cpus.size();) {
            size_t j = i;
            while (j + 1 < cpus.size() && cpus[j + 1] == cpus[j] + 1) ++j;
            if (!out.empty()) out += ",";
            out += std::to_string(cpus[i]);
            if (j > i) out += "-" + std::to_string(cpus[j]);
            i = j + 1;
        }
        return out.empty() ? "no cpu" : "cpu " + out;
    }

private:
    std::vector<Cluster> clusters_;
    double homeFactor_ = 1.0;
    int home_ = -1, current_ = -1, candidate_ = -1;
    unsigned streak_ = 0, sinceMove_ = 0, moves_ = 0;
};
