#pragma once
// Look-ahead peak limiter for the host mix (guest_audio.inc runs it on every AAudio burst after the mixer).
//
// The game's streams (music, engine, effects) sum past full scale in races: peaks of 1.29 in the build 2-8
// sessions, tens of thousands of samples a session over 1.0. Clamping them flattened the waveform tops into
// crackle. Here the output is the input delayed by a short look-ahead (2 ms) and scaled by a gain that is
// already low enough when a peak arrives:
//   - need(n): ceiling / (largest |sample| among input frames n-D .. n), at most 1 (a sliding maximum);
//   - the gain follows the mean of the last D needs, so it ramps down across the look-ahead instead of
//     stepping; every one of those windows contains the frame being output, so the mean never lets that
//     frame out above the ceiling;
//   - it recovers with a one-pole release (250 ms) and drops at once when the ramp asks for less.
//     The recursion runs in double: in float, each step near unity (8e-5 of the distance left) falls
//     below half a float's spacing once the gain is within 4e-4 of 1, and the gain stopped there for
//     good (build 9: 0.9996 through silences, read as "limited" by the session counters).
// Both channels share the gain, so the stereo image does not move. A mix that stays under the ceiling is
// passed through unchanged, only delayed. A final clamp keeps the output in range whatever the input
// (NaN or infinity from a broken voice become silence).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

class PeakLimiter {
public:
    static constexpr float kCeiling = 0.966f;   // -0.3 dBFS

    void configure(unsigned rate, float lookaheadMs = 2.0f, float releaseMs = 250.0f) {
        rate_ = rate ? rate : 48000u;
        lookahead_ = std::max(1u, unsigned(std::lround(rate_ * lookaheadMs / 1000.0f)));
        release_ = 1.0 - std::exp(-1.0 / (double(releaseMs) / 1000.0 * double(rate_)));
        delay_.assign(size_t(lookahead_) * 2, 0.0f);
        needs_.assign(lookahead_, 1.0f);
        peaks_.assign(size_t(lookahead_) + 1, Peak{});
        head_ = tail_ = size_ = 0;
        needSum_ = double(lookahead_);
        gain_ = 1.0;
        frame_ = 0;
        limitedFrames = 0;
    }

    unsigned lookaheadFrames() const { return lookahead_; }
    float gain() const { return float(gain_); }
    // Output frames the limiter turned down (gain below 1); the diagnostics thread reads and resets it.
    std::atomic<uint64_t> limitedFrames{0};

    // In place: `frames` interleaved stereo frames in, the same number (delayed by lookaheadFrames()) out.
    void process(float* stereo, unsigned frames) {
        if (!lookahead_) configure(48000);
        for (unsigned i = 0; i < frames; ++i) {
            float left = stereo[2 * i], right = stereo[2 * i + 1];
            if (!std::isfinite(left)) left = 0.0f;
            if (!std::isfinite(right)) right = 0.0f;
            float peak = std::max(std::fabs(left), std::fabs(right));
            // Sliding maximum over input frames frame-lookahead .. frame (a monotonic queue of (frame, peak) with
            // room for lookahead+1 entries: the expired ones leave before the new one enters).
            const size_t cap = peaks_.size();
            while (size_ && peaks_[head_].frame + lookahead_ < frame_) { head_ = (head_ + 1) % cap; --size_; }
            while (size_ && peaks_[(tail_ + cap - 1) % cap].peak <= peak) { tail_ = (tail_ + cap - 1) % cap; --size_; }
            peaks_[tail_] = {frame_, peak};
            tail_ = (tail_ + 1) % cap; ++size_;
            float windowPeak = peaks_[head_].peak;
            float need = windowPeak > kCeiling ? kCeiling / windowPeak : 1.0f;
            // Mean of the last `lookahead` needs: a ramp across the look-ahead.
            size_t slot = size_t(frame_ % lookahead_);
            needSum_ += double(need) - double(needs_[slot]);
            needs_[slot] = need;
            double ramp = needSum_ / double(lookahead_);
            if (ramp > 1.0) ramp = 1.0;
            gain_ = ramp < gain_ ? ramp : gain_ + (ramp - gain_) * release_;
            if (gain_ < 0.9999) limitedFrames.fetch_add(1, std::memory_order_relaxed);
            const float gain = float(gain_);
            // The frame from `lookahead` frames ago leaves, the new one waits.
            float outLeft = delay_[2 * slot], outRight = delay_[2 * slot + 1];
            delay_[2 * slot] = left; delay_[2 * slot + 1] = right;
            stereo[2 * i] = std::clamp(outLeft * gain, -1.0f, 1.0f);
            stereo[2 * i + 1] = std::clamp(outRight * gain, -1.0f, 1.0f);
            ++frame_;
            // The running sum drifts by rounding; rebuild it once a second.
            if (frame_ % rate_ == 0) { needSum_ = 0; for (float value : needs_) needSum_ += value; }
        }
    }

private:
    struct Peak { uint64_t frame = 0; float peak = 0.0f; };
    unsigned rate_ = 0, lookahead_ = 0;
    double release_ = 0.0, gain_ = 1.0;
    double needSum_ = 0.0;
    uint64_t frame_ = 0;
    std::vector<float> delay_, needs_;
    std::vector<Peak> peaks_;
    size_t head_ = 0, tail_ = 0, size_ = 0;
};
