// The look-ahead peak limiter after the host mix (android/app/src/main/cpp/audio_limiter.h).
#include "../android/app/src/main/cpp/audio_limiter.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
static void require(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
int main() {
    constexpr double pi = 3.14159265358979323846;
    const unsigned rate = 48000;
    {
        // Under the ceiling: delayed by the look-ahead, otherwise bit for bit.
        PeakLimiter limiter; limiter.configure(rate);
        unsigned d = limiter.lookaheadFrames();
        require(d == 96, "2 ms at 48 kHz");
        std::vector<float> in(2 * 4000), out;
        for (unsigned i = 0; i < 4000; ++i) { in[2 * i] = float(0.9 * std::sin(2 * pi * 440 * i / rate)); in[2 * i + 1] = float(-0.5 * std::sin(2 * pi * 330 * i / rate)); }
        out = in; limiter.process(out.data(), 4000);
        for (unsigned i = 0; i < d; ++i) require(out[2 * i] == 0 && out[2 * i + 1] == 0, "the look-ahead starts silent");
        for (unsigned i = d; i < 4000; ++i) require(out[2 * i] == in[2 * (i - d)] && out[2 * i + 1] == in[2 * (i - d) + 1], "a quiet mix passes unchanged");
        require(limiter.limitedFrames.load() == 0 && limiter.gain() == 1.0f, "the limiter stays out of the way");
    }
    {
        // A race's worst: a 100 Hz tone at 1.29 plus a 2 kHz component, in bursts. Never above the ceiling, no
        // flat tops (the gain ramps down before the peak instead of clipping it), and the gain recovers.
        PeakLimiter limiter; limiter.configure(rate);
        unsigned frames = rate * 2;
        std::vector<float> buffer(2 * frames);
        for (unsigned i = 0; i < frames; ++i) {
            bool loud = (i / (rate / 4)) % 2 == 0;   // 250 ms loud, 250 ms quiet
            double v = (loud ? 1.1 : 0.3) * std::sin(2 * pi * 100 * i / rate) + (loud ? 0.19 : 0.05) * std::sin(2 * pi * 2000 * i / rate);
            buffer[2 * i] = float(v); buffer[2 * i + 1] = float(0.8 * v);
        }
        std::vector<float> original = buffer;
        // In bursts like AAudio's callbacks.
        for (unsigned at = 0; at < frames; at += 192) limiter.process(buffer.data() + 2 * at, std::min(192u, frames - at));
        float top = 0; unsigned plateau = 0;
        for (unsigned i = 1; i + 1 < frames; ++i) {
            top = std::max(top, std::fabs(buffer[2 * i]));
            if (std::fabs(buffer[2 * i]) > 0.95f && buffer[2 * i] == buffer[2 * i - 2] && buffer[2 * i] == buffer[2 * i + 2]) ++plateau;
        }
        require(top <= PeakLimiter::kCeiling + 1e-6f, "never above the ceiling");
        require(top > 0.9f, "the limited mix still reaches the ceiling region");
        require(plateau == 0, "no clamped plateaus");
        require(limiter.limitedFrames.load() > 0, "the loud bursts were turned down");
        // The output is the input times a gain between 0 and 1 (no distortion beyond the gain): check the shape
        // by the ratio to the delayed input wherever the input is not tiny.
        unsigned d = limiter.lookaheadFrames(); unsigned bad = 0;
        for (unsigned i = d; i < frames; ++i) {
            float x = original[2 * (i - d)], y = buffer[2 * i];
            if (std::fabs(x) < 0.05f) continue;
            float g = y / x;
            if (!(g > 0.0f && g <= 1.0f + 1e-6f)) ++bad;
        }
        require(bad == 0, "a pure gain: same sign, never louder than the input");
        // Quiet at the end: the gain has recovered within a 250 ms quiet burst.
        float lastGain = limiter.gain();
        require(lastGain > 0.6f, "recovering during the quiet burst");
    }
    {
        // Broken input never escapes: NaN and infinity become silence, huge values are limited.
        PeakLimiter limiter; limiter.configure(rate);
        std::vector<float> buffer(2 * 400, 0.0f);
        buffer[2 * 10] = NAN; buffer[2 * 11 + 1] = INFINITY; buffer[2 * 50] = 1000.0f;
        limiter.process(buffer.data(), 400);
        for (float v : buffer) require(std::isfinite(v) && std::fabs(v) <= PeakLimiter::kCeiling + 1e-6f, "always finite and in range");
    }
    std::puts("audio_limiter: ok");
    return 0;
}
