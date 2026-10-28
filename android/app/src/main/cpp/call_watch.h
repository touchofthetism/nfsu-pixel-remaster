#pragma once
// Long D3D9 calls, caught by the profiler's sampler (guest_runtime.cpp).
//
// Build 9's slow-frame lines showed 80-110 ms frames on a warm phone spent almost entirely inside one D3D9
// call of the game's frame function (0x40A580: Present, BeginScene/EndScene, Clear or
// TestCooperativeLevel), without saying which call or what it waited for. Timing every call would cost two
// clock reads on each of 50 thousand calls a frame, so the thread making the calls only publishes which one
// it is in (a token and a sequence number, two relaxed stores) and the sampler, already awake every
// millisecond, watches for one that stays: past the threshold it takes a snapshot of whatever explains a
// wait (DXVK's queue, its threads' states), and when the call ends it reports the call, how long it lasted
// (to the sampling interval) and the snapshot, and adds it to per-method totals for the session record.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>

class CallWatch {
public:
    struct Call {
        uint32_t token = 0, func = 0;   // the call, and the lifted function that made it
        uint64_t startNs = 0, ns = 0;   // first sample that saw it, and how long it was seen
        std::string detail;             // the snapshot taken when it crossed the threshold
    };

    explicit CallWatch(uint64_t thresholdNs = 20000000ull) : threshold_(thresholdNs) {}

    // The calling side: one begin and one end per call. D3D9 calls are serialised by the guest machine lock,
    // so the stores never race each other; the sampler only reads.
    void begin(uint32_t token) {
        seq_.store(seq_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        token_.store(token, std::memory_order_relaxed);
    }
    void end() { token_.store(0, std::memory_order_relaxed); }

    // What to record when a call crosses the threshold (called from the sampler thread).
    void setSnapshot(std::function<std::string()> snapshot) { snapshot_ = std::move(snapshot); }

    // The sampler, once per interval, with the time and the caller (g_cur_func) it sees. True when a call that
    // reached the threshold has just ended; *done then holds it (and the per-method totals include it).
    bool sample(uint64_t nowNs, uint32_t func, Call* done) {
        uint32_t token = token_.load(std::memory_order_relaxed), seq = seq_.load(std::memory_order_relaxed);
        if (token && token == current_.token && seq == currentSeq_) {
            current_.ns = nowNs - current_.startNs;
            if (!crossed_ && current_.ns >= threshold_) {
                crossed_ = true;
                if (snapshot_) current_.detail = snapshot_();
            }
            return false;
        }
        bool finished = current_.token && crossed_;
        if (finished) {
            *done = current_;
            std::lock_guard<std::mutex> lock(mutex_);
            Totals& t = totals_[current_.token];
            ++t.count; t.ns += current_.ns; t.maxNs = std::max(t.maxNs, current_.ns);
        }
        current_ = Call{};
        current_.token = token; current_.func = func; current_.startNs = nowNs;
        currentSeq_ = seq; crossed_ = false;
        return finished;
    }

    // {"2/17":[count,totalMs,maxMs],...}: tokens as d3d9MethodTimeJson names them ((token - base) / 16 split
    // into kind / method).
    std::string json(uint32_t methodBase) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string out = "{";
        char item[80];
        for (const auto& entry : totals_) {
            uint32_t index = ((entry.first - methodBase) / 16) & 4095;
            std::snprintf(item, sizeof item, "%s\"%u/%u\":[%llu,%.1f,%.1f]", out.size() > 1 ? "," : "", index / 256, index % 256,
                          static_cast<unsigned long long>(entry.second.count), entry.second.ns / 1e6, entry.second.maxNs / 1e6);
            out += item;
        }
        return out + "}";
    }

    // The sampler skipped time (the game was paused): whatever call was in flight is not timed across it.
    void reset() { current_ = Call{}; currentSeq_ = 0; crossed_ = false; }

    uint64_t thresholdNs() const { return threshold_; }

private:
    struct Totals { uint64_t count = 0, ns = 0, maxNs = 0; };
    const uint64_t threshold_;
    std::atomic<uint32_t> token_{0}, seq_{0};
    // Sampler-side state.
    Call current_;
    uint32_t currentSeq_ = 0;
    bool crossed_ = false;
    std::function<std::string()> snapshot_;
    mutable std::mutex mutex_;
    std::map<uint32_t, Totals> totals_;
};
