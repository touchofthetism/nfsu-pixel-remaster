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
// checks (5 % faster since build 16). Clusters of less than half the home cluster's capacity (the little cores) are never chosen. A cap that
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
//
// Build 16: a cluster does not always run at its cap. In build 15's 69-minute session of 2026-10-28 22:25 (thermal
// status light from the second minute, the battery down to 4 %), the X4 was allowed 1152-1420 MHz from the 40th
// minute, and while the render thread ran on it, it ran at 883 MHz in 72 % of the 2 s samples: 75 % of its cap
// on average (988 of 1323 MHz). The A725s ran at 88 % of theirs with the thread (1877 of 2132, at 1536 when below),
// and both ran at their caps once it had left. That is Pixel's PMU frequency limit (sched_pixel's limit_frequency
// for work that stalls on memory past spc_threshold, switched on by the power HAL from thermal status light: AOSP's
// powerhint files set PMU_POLL there); scaling_max_freq does not show it, and the game's code is memory-bound. On
// the caps alone the thread shuttled 85 times, about every 20 s, and it did better on cpu 2-4: in 64 of the 76
// stretches between moves the frame rate differed from the neighbouring stretches on the other cluster in the
// mids' favour (median 10.6 %; 61.1 against 54.9 FPS over the 39 stretches of each).
//
// So each cluster's speed is also scaled by its share of its cap: the clock it ran at with the render thread
// on it (cpufreq's time_in_state over the check), over its cap, learned a quarter at a time while the thread ran
// most of the check (75 %) and its frames came slower than the target rate (90 %): the clock it got is then what
// the cluster gave it, not what it needed. The first three checks after a move do not count (the clock follows
// the new load within about 4 s), and a share relaxes back to 1 with a five-minute time constant, so a cluster
// left for running slow is tried again now and then; all shares start over when the thermal status changes (the
// PMU limit is switched with it). The home cluster now takes the thread back once it is 5 % faster (it was as
// fast before), since both sides of the comparison are measured. Replayed through the 29 minutes of caps the
// session recorded after the 40th minute (tests/data), build 15's rule moves the thread 79 times (the session did
// 78) and leaves it on the X4 59 % of the time; this one moves it 25 times, 8 % of the time on the X4, and with
// the clocks the session measured the thread gets 10.4 % more (11.7 % had it never left cpu 2-4). With the X4
// allowed 2208 MHz again and running at it (11 % faster than cpu 2-4 then) the thread is home after 4-5 minutes.
//
// DXVK's command stream thread (build 16) goes to the fastest eligible cluster other than the render thread's, by
// the clock its cluster is allowed (the share the render thread's load was measured to get does not say what this
// thread's gets). It does the Vulkan half of every draw, yet the scheduler kept it on the most throttled cluster:
// 8 of the 9 long frames the log describes in that session's hot part found it on cpu 5-6, allowed 0.9-1.3 GHz
// while cpu 2-4 were allowed 1.8-2.3, and in two of them the render thread was waiting for it with the GPU idle.
// It moves like the render thread leaving home (20 % faster for three checks, ten after its last move), and at
// once when the render thread takes its cluster. DXVK's other threads keep helperCpus.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// What the render thread's cluster gave it since the last check (host_perf.cpp measures it): the clock the cluster
// ran at on average (kHz; 0 = not measured), the share of the check the thread was on a cpu, and whether its frames
// came slower than 90 % of the target rate. (At namespace scope: a default argument of CoreChoice::check.)
struct CoreChoiceObservation {
    unsigned clockKhz = 0;
    double busy = 0;
    bool behind = false;
};

class CoreChoice {
public:
    struct Cluster {
        std::vector<int> cpus;     // the cpus sharing one clock (cpufreq policy)
        unsigned capacity = 0;     // cpu_capacity of its cpus (1024 = the biggest core at its top clock)
        unsigned maxKhz = 0;       // cpuinfo_max_freq
    };
    using Observation = CoreChoiceObservation;

    // Thresholds (see above).
    static constexpr double kLeaveFactor = 1.20;    // another cluster this much faster ...
    static constexpr unsigned kLeaveChecks = 3;      // ... for this many checks in a row
    static constexpr double kReturnFactor = 1.05;   // the home cluster this much faster (build 16; 1.00 before) ...
    static constexpr unsigned kReturnChecks = 5;     // ... for this many checks in a row
    static constexpr unsigned kDwellChecks = 10;     // checks between two moves at least
    static constexpr double kEligible = 0.5;         // a cluster's capacity against the home cluster's
    static constexpr double kHomeWorkPerClock = 1.5; // the home cluster's work per clock against the others', at least
    // Build 16: a cluster's share of its cap (see above).
    static constexpr double kLearnBusy = 0.75;       // the render thread on a cpu this share of the check at least
    static constexpr unsigned kSettleChecks = 3;     // checks after a move before the new cluster's clock counts
    static constexpr double kLearnWeight = 0.25;     // one check's weight in the share
    static constexpr double kRelaxChecks = 300;      // time constant (checks) of a share's way back to 1
    static constexpr double kMinShare = 0.2;         // a share never counts as less

    CoreChoice() = default;
    // `returnFactor`: kReturnFactor; tests replay build 15's rule with 1.0.
    explicit CoreChoice(std::vector<Cluster> clusters, double returnFactor = kReturnFactor)
        : clusters_(std::move(clusters)), returnFactor_(returnFactor) {
        for (size_t i = 0; i < clusters_.size(); ++i) {
            const Cluster& c = clusters_[i];
            const Cluster* h = home_ < 0 ? nullptr : &clusters_[size_t(home_)];
            if (!h || c.capacity > h->capacity || (c.capacity == h->capacity && c.maxKhz > h->maxKhz)) home_ = int(i);
        }
        current_ = home_;
        sinceMove_ = kDwellChecks;
        share_.assign(clusters_.size(), 1.0);
        learned_.assign(clusters_.size(), 0);
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
        // The command stream's cluster: the fastest other than the render thread's, at the top clocks.
        if (home_ >= 0) {
            std::vector<unsigned> top(clusters_.size(), 0);
            double unused = 0;
            stream_ = fastestBesides(home_, top, &unused);
            streamSince_ = kDwellChecks;
        }
    }

    size_t size() const { return clusters_.size(); }
    const Cluster& cluster(size_t i) const { return clusters_[i]; }
    int home() const { return home_; }
    int current() const { return current_; }
    unsigned moves() const { return moves_; }
    // A cluster's share of its cap under the render thread's load (1 until measured), and how many checks
    // measured it.
    double share(size_t i) const { return i < share_.size() ? share_[i] : 1.0; }
    unsigned learned(size_t i) const { return i < learned_.size() ? learned_[i] : 0; }
    // Conditions changed (the thermal status): every share starts over at 1.
    void resetShares() { std::fill(share_.begin(), share_.end(), 1.0); }
    // The command stream thread's cluster (-1: no eligible cluster besides the render thread's) and its moves.
    int stream() const { return stream_; }
    unsigned streamMoves() const { return streamMoves_; }
    // What the home cluster's capacity is multiplied by in speed(): 1 where the kernel already credits it with
    // kHomeWorkPerClock times the others' work per clock or more.
    double homeFactor() const { return homeFactor_; }

    bool eligible(size_t i) const {
        return home_ >= 0 && clusters_[i].capacity >= kEligible * clusters_[size_t(home_)].capacity;
    }

    // The cluster's speed for the render thread with the given cap (kHz; 0 or above the top clock: the top
    // clock): at the cap, times the share of it the cluster gave the thread.
    double speed(size_t i, unsigned capKhz) const { return speedAtCap(i, capKhz) * share(i); }
    // The same at the cap itself (the command stream's speed).
    double speedAtCap(size_t i, unsigned capKhz) const {
        const Cluster& c = clusters_[i];
        double factor = int(i) == home_ ? homeFactor_ : 1.0;
        if (!c.maxKhz) return c.capacity * factor;
        return double(c.capacity) * allowedKhz(i, capKhz) / c.maxKhz * factor;
    }
    // The clock a cluster may run at: its cap, or its top clock where the cap is unknown (0) or above it.
    unsigned allowedKhz(size_t i, unsigned capKhz) const {
        unsigned top = clusters_[i].maxKhz;
        return capKhz && (!top || capKhz < top) ? capKhz : top;
    }

    // One check with each cluster's cap (scaling_max_freq, kHz, in cluster order; 0 = unknown) and what the
    // render thread's cluster gave it since the last check. Returns true when the thread should move now
    // (current() is then the new cluster).
    bool check(const std::vector<unsigned>& capKhz, const Observation& seen = Observation()) {
        if (home_ < 0 || capKhz.size() != clusters_.size()) return false;
        ++sinceMove_;
        learn(capKhz, seen);
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
            if (best == home_ && bestSpeed >= returnFactor_ * now) {
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

    // After check(): the command stream thread's turn. Returns true when it should move now (stream() is then
    // its new cluster).
    bool checkStream(const std::vector<unsigned>& capKhz) {
        if (stream_ < 0 || capKhz.size() != clusters_.size()) return false;
        ++streamSince_;
        double bestSpeed = 0;
        int best = fastestBesides(current_, capKhz, &bestSpeed);
        if (best < 0) return false;
        int target = -1;
        if (stream_ == current_) target = best;   // the render thread took its cluster: at once
        else if (best != stream_ && bestSpeed >= kLeaveFactor * speedAtCap(size_t(stream_), capKhz[size_t(stream_)])) {
            if (best != streamCandidate_) { streamCandidate_ = best; streamStreak_ = 0; }
            if (++streamStreak_ >= kLeaveChecks && streamSince_ >= kDwellChecks) target = best;
        } else {
            streamCandidate_ = -1; streamStreak_ = 0;
        }
        if (target < 0) return false;
        stream_ = target;
        streamCandidate_ = -1; streamStreak_ = 0; streamSince_ = 0; ++streamMoves_;
        return true;
    }

    // The cpus for DXVK's threads while the render thread is on `cluster`: every other cluster's (they must not
    // take turns with it), and not the home cluster's either while the render thread has left it for being slow,
    // as long as another cluster of the eligible size remains for them. Builds 12-15 put the command stream here
    // too; since build 16 it has a cluster of its own (stream()), often the home cluster then: one core on most
    // phones, which these threads leave to it. On a phone with one mid cluster this would leave only the little
    // cores, so there the home cluster stays allowed.
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
    // Every share relaxes towards 1; the render thread's cluster then learns from what it gave the thread, once
    // it has settled after a move, while the thread was busy and behind its target rate.
    void learn(const std::vector<unsigned>& capKhz, const Observation& seen) {
        for (double& s : share_) s = 1.0 - (1.0 - s) * (1.0 - 1.0 / kRelaxChecks);
        if (sinceMove_ <= kSettleChecks || !seen.clockKhz || !seen.behind || seen.busy < kLearnBusy) return;
        unsigned cap = allowedKhz(size_t(current_), capKhz[size_t(current_)]);
        if (!cap) return;
        double r = std::min(1.0, std::max(kMinShare, double(seen.clockKhz) / cap));
        share_[size_t(current_)] += kLearnWeight * (r - share_[size_t(current_)]);
        ++learned_[size_t(current_)];
    }
    // The eligible cluster other than `besides` with the highest speed at its cap (ties: the higher cpus, as in
    // check()); -1 if none.
    int fastestBesides(int besides, const std::vector<unsigned>& capKhz, double* bestSpeed) const {
        int best = -1;
        for (size_t k = clusters_.size(); k-- > 0;) {
            if (int(k) == besides || !eligible(k)) continue;
            double s = speedAtCap(k, capKhz[k]);
            if (best < 0 || s > *bestSpeed) { best = int(k); *bestSpeed = s; }
        }
        return best;
    }

    std::vector<Cluster> clusters_;
    double returnFactor_ = kReturnFactor;
    double homeFactor_ = 1.0;
    int home_ = -1, current_ = -1, candidate_ = -1;
    unsigned streak_ = 0, sinceMove_ = 0, moves_ = 0;
    std::vector<double> share_;
    std::vector<unsigned> learned_;
    int stream_ = -1, streamCandidate_ = -1;
    unsigned streamStreak_ = 0, streamSince_ = 0, streamMoves_ = 0;
};
