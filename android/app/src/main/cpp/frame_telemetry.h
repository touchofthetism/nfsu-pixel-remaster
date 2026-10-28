#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

// Frame telemetry for the game's Present calls: one writer (the guest render thread) and occasional
// readers (the overlay every half second, the session recorder every 30 s). Per frame it costs one
// uncontended mutex and a handful of integer operations; the JSON is built only when asked for.
//
// Everything is derived from the interval between consecutive Presents (what the player sees) and the
// time spent inside Present (DXVK + the swapchain: a proxy for GPU-bound or vsync-bound frames). With a GPU
// idle counter (gpuIdleSource: microseconds the GPU had nothing in flight, DXVK's "GPU load" measure) each
// second also records how busy the GPU was, which tells a GPU-bound second from a CPU-bound one.
class FrameTelemetry {
public:
    using Clock = std::chrono::steady_clock;
    using Nanos = std::chrono::nanoseconds;

    // Upper bounds (microseconds) of the frame-interval histogram; the last bucket is open-ended.
    static constexpr std::array<uint32_t, 25> kBoundsUs = {
        4000, 6000, 7000, 8000, 8500, 9000, 10000, 11000, 12000, 14000, 16000, 16700, 18000, 20000,
        25000, 33333, 40000, 50000, 66667, 100000, 150000, 250000, 500000, 1000000, 0xFFFFFFFFu};
    // A frame slower than this is a pause (app in the background, a loading screen with no frames),
    // not a stutter; it is counted separately and does not enter the distribution.
    static constexpr uint32_t kGapUs = 2000000;
    // Frames slower than this are stalls whatever the target frame rate (a shader compile, a disk read).
    static constexpr uint32_t kStallUs = 100000;
    static constexpr size_t kRecentFrames = 240;
    static constexpr size_t kMaxSeconds = 4 * 3600;

    struct Summary {
        uint64_t frames = 0, hitches = 0, stalls = 0, gaps = 0, spikes = 0;
        double activeSeconds = 0, avgFps = 0, p50Ms = 0, p90Ms = 0, p99Ms = 0, p999Ms = 0, maxMs = 0, presentMsAvg = 0;
        unsigned targetFps = 0;
        // Mean of the measured seconds' GPU busy percentage; -1 without a GPU idle source.
        double gpuBusyAvg = -1;
    };

    // Microseconds of GPU idle time so far (monotonic); set once, before the first Present.
    using GpuIdleMicros = uint64_t (*)();
    void gpuIdleSource(GpuIdleMicros source) {
        std::lock_guard<std::mutex> lock(mutex_);
        gpuIdle_ = source;
    }

    // Called with the Present's completion time, the time spent inside Present and the frame cap
    // (0 = uncapped; 120 is then assumed for the hitch threshold).
    void present(Clock::time_point now, Nanos presentSpent, unsigned targetFps) {
        std::lock_guard<std::mutex> lock(mutex_);
        targetFps_ = targetFps;
        presentNs_ += uint64_t(std::max<int64_t>(0, presentSpent.count()));
        ++presentCalls_;
        if (!started_) { started_ = true; start_ = now; }
        if (last_.time_since_epoch().count() == 0) { last_ = now; return; }
        auto interval = std::chrono::duration_cast<std::chrono::microseconds>(now - last_).count();
        last_ = now;
        if (interval < 0) return;
        uint32_t us = interval > int64_t(0xFFFFFFFFu) ? 0xFFFFFFFFu : uint32_t(interval);
        if (us >= kGapUs) { ++gaps_; secondOpen_ = false; return; }
        record(us, uint32_t(std::chrono::duration_cast<std::chrono::seconds>(now - start_).count()), now);
    }

    // The app paused: the next Present starts a new interval instead of measuring the pause.
    void pause() {
        std::lock_guard<std::mutex> lock(mutex_);
        last_ = {};
        secondOpen_ = false;   // the GPU sat idle in the background: that second is not measured
    }

    Summary summary() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return summaryLocked();
    }

    // The complete record for the session file: totals, distribution, one entry per active second.
    std::string json() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string out = summaryJsonLocked();
        out += ",\"histogramUs\":[";
        bool first = true;
        for (size_t i = 0; i < kBoundsUs.size(); ++i) {
            if (!histogram_[i]) continue;
            char item[64];
            std::snprintf(item, sizeof item, "%s[%u,%llu]", first ? "" : ",", kBoundsUs[i],
                static_cast<unsigned long long>(histogram_[i]));
            out += item;
            first = false;
        }
        // [second, frames, worst frame ms, GPU busy % (-1: not measured)]
        out += "],\"perSecond\":[";
        for (size_t i = 0; i < seconds_.size(); ++i) {
            char item[64];
            std::snprintf(item, sizeof item, "%s[%u,%u,%.1f,%d]", i ? "," : "", seconds_[i].second, seconds_[i].frames,
                seconds_[i].maxUs / 1000.0, int(seconds_[i].gpuBusy));
            out += item;
        }
        out += "]}";
        return out;
    }

    // What the overlay shows: the last 240 frames and the session totals, without the per-second series.
    std::string liveJson() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string out = summaryJsonLocked();
        std::vector<uint32_t> recent(recent_.begin(), recent_.begin() + std::min(recentCount_, kRecentFrames));
        double fps = 0, p99 = 0, maxMs = 0;
        if (!recent.empty()) {
            uint64_t sum = 0;
            for (auto v : recent) sum += v;
            fps = sum ? recent.size() * 1e6 / double(sum) : 0;
            std::sort(recent.begin(), recent.end());
            p99 = recent[std::min(recent.size() - 1, size_t(recent.size() * 99 / 100))] / 1000.0;
            maxMs = recent.back() / 1000.0;
        }
        char item[128];
        std::snprintf(item, sizeof item, ",\"recent\":{\"frames\":%zu,\"fps\":%.1f,\"p99Ms\":%.2f,\"maxMs\":%.2f}}",
            recent.size(), fps, p99, maxMs);
        out += item;
        return out;
    }

private:
    struct Second { uint32_t second, frames, maxUs; int8_t gpuBusy; };

    // Closes the open second's GPU measure at `now` (a frame of the next second has just been presented).
    void closeSecondLocked(Clock::time_point now) {
        if (!secondOpen_ || !gpuIdle_ || seconds_.empty()) return;
        secondOpen_ = false;
        int64_t wall = std::chrono::duration_cast<std::chrono::microseconds>(now - secondStart_).count();
        uint64_t idle = gpuIdle_() - secondStartIdle_;
        if (wall < 500000) return;   // a partial second (start, after a pause) says little
        double busy = 100.0 * (1.0 - double(idle) / double(wall));
        seconds_.back().gpuBusy = int8_t(busy < 0 ? 0 : busy > 100 ? 100 : int(busy + 0.5));
    }

    void record(uint32_t us, uint32_t second, Clock::time_point now) {
        ++frames_;
        totalUs_ += us;
        maxUs_ = std::max(maxUs_, us);
        unsigned target = targetFps_ ? targetFps_ : 120;
        uint32_t hitchUs = std::max<uint32_t>(25000, 2500000u / target);  // 2.5 frame periods
        if (us > hitchUs) ++hitches_;
        if (us > kStallUs) ++stalls_;
        // Spikes: frames much slower than the frames around them, whatever the cap (the cap may be
        // unreachable, and then "hitches" counts nearly everything). Typical = exponential average.
        if (typicalUs_ > 0 && us > typicalUs_ * 5 / 2 && us > 12000) ++spikes_;
        typicalUs_ = typicalUs_ ? (typicalUs_ * 15 + us) / 16 : us;
        size_t bucket = size_t(std::lower_bound(kBoundsUs.begin(), kBoundsUs.end(), us) - kBoundsUs.begin());
        ++histogram_[std::min(bucket, kBoundsUs.size() - 1)];
        recent_[recentNext_] = us;
        recentNext_ = (recentNext_ + 1) % kRecentFrames;
        ++recentCount_;
        if (seconds_.empty() || seconds_.back().second != second) {
            closeSecondLocked(now);
            if (seconds_.size() < kMaxSeconds) seconds_.push_back({second, 0, 0, -1});
            else return;
            if (gpuIdle_) { secondOpen_ = true; secondStart_ = now; secondStartIdle_ = gpuIdle_(); }
        }
        auto& current = seconds_.back();
        ++current.frames;
        current.maxUs = std::max(current.maxUs, us);
    }

    double percentileMsLocked(double quantile) const {
        if (!frames_) return 0;
        uint64_t wanted = uint64_t(double(frames_) * quantile), seen = 0;
        if (wanted >= frames_) wanted = frames_ - 1;
        for (size_t i = 0; i < kBoundsUs.size(); ++i) {
            seen += histogram_[i];
            if (seen > wanted) return (i + 1 == kBoundsUs.size() ? maxUs_ : kBoundsUs[i]) / 1000.0;
        }
        return maxUs_ / 1000.0;
    }

    Summary summaryLocked() const {
        Summary s;
        s.frames = frames_; s.hitches = hitches_; s.stalls = stalls_; s.gaps = gaps_; s.spikes = spikes_; s.targetFps = targetFps_;
        s.activeSeconds = totalUs_ / 1e6;
        s.avgFps = totalUs_ ? frames_ * 1e6 / double(totalUs_) : 0;
        s.p50Ms = percentileMsLocked(.5); s.p90Ms = percentileMsLocked(.9);
        s.p99Ms = percentileMsLocked(.99); s.p999Ms = percentileMsLocked(.999);
        s.maxMs = maxUs_ / 1000.0;
        s.presentMsAvg = presentCalls_ ? presentNs_ / 1e6 / double(presentCalls_) : 0;
        uint64_t busySum = 0, measured = 0;
        for (const auto& second : seconds_) if (second.gpuBusy >= 0) { busySum += uint64_t(second.gpuBusy); ++measured; }
        if (measured) s.gpuBusyAvg = double(busySum) / double(measured);
        return s;
    }

    // Opens the object; callers append their own members and close it.
    std::string summaryJsonLocked() const {
        Summary s = summaryLocked();
        char text[512];
        std::snprintf(text, sizeof text,
            "{\"frames\":%llu,\"activeSeconds\":%.1f,\"avgFps\":%.1f,\"p50Ms\":%.2f,\"p90Ms\":%.2f,\"p99Ms\":%.2f,"
            "\"p999Ms\":%.2f,\"maxMs\":%.2f,\"onePercentLowFps\":%.1f,\"hitches\":%llu,\"stalls\":%llu,\"gaps\":%llu,\"spikes\":%llu,"
            "\"presentMsAvg\":%.2f,\"targetFps\":%u,\"gpuBusyAvg\":%.1f",
            static_cast<unsigned long long>(s.frames), s.activeSeconds, s.avgFps, s.p50Ms, s.p90Ms, s.p99Ms, s.p999Ms,
            s.maxMs, s.p99Ms > 0 ? 1000.0 / s.p99Ms : 0.0, static_cast<unsigned long long>(s.hitches),
            static_cast<unsigned long long>(s.stalls), static_cast<unsigned long long>(s.gaps), static_cast<unsigned long long>(s.spikes), s.presentMsAvg, s.targetFps,
            s.gpuBusyAvg);
        return text;
    }

    mutable std::mutex mutex_;
    GpuIdleMicros gpuIdle_ = nullptr;
    bool secondOpen_ = false;
    Clock::time_point secondStart_{};
    uint64_t secondStartIdle_ = 0;
    bool started_ = false;
    Clock::time_point start_{}, last_{};
    unsigned targetFps_ = 0;
    uint64_t frames_ = 0, hitches_ = 0, stalls_ = 0, gaps_ = 0, spikes_ = 0, totalUs_ = 0, presentNs_ = 0, presentCalls_ = 0;
    uint32_t maxUs_ = 0, typicalUs_ = 0;
    std::array<uint64_t, kBoundsUs.size()> histogram_{};
    std::array<uint32_t, kRecentFrames> recent_{};
    size_t recentNext_ = 0, recentCount_ = 0;
    std::vector<Second> seconds_;
};
