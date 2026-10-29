#pragma once
// The game's sound output (guest_audio.inc): one AAudio stream at a time, started and paused with the game, and
// replaced whenever Android takes it away. tests/sound_output.cpp runs it against a simulated AAudio.
//
// A stream can stop working for good at any moment. The audio server takes a shared MMAP stream back while the
// app is in the background: build 14 on the Pixel, after 40 s and after 85 min away, had requestStart answer
// AAUDIO_ERROR_DISCONNECTED ("stream was probably stolen") and every later start AAUDIO_ERROR_INVALID_STATE, and
// the game ran on without sound until it was closed. A change of output device (headphones in or out, a
// Bluetooth headset) disconnects a playing stream, which AAudio reports once, through the error callback.
//
// Here a stream that fails to start or pause, reports an error, or shows itself disconnected is closed and
// another opened in its place: at once while the game plays, at the resume while it is paused (a stream opened in
// the background would be taken back the same way). AAudio allows no close or open from its own threads, so
// markLost (the error callback) only raises a flag, and the caller wakes whoever runs service(). Every other
// member runs under the caller's lock. A failed open is tried again after 0.5 s, then after twice the wait each
// time, up to 8 s; a stream lost within 5 s of opening counts as a failed open (a device that keeps dropping
// streams is not hammered), and one that lasted longer starts the waits over.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>

// The waits (above). Tests of the whole device (tests/sound_device.cpp) shorten them.
struct SoundOutputTiming { uint64_t firstRetryMs = 500, lastRetryMs = 8000, stableMs = 5000; };

class SoundOutput {
public:
    using Timing = SoundOutputTiming;
    using Stream = void*;
    struct System {
        std::function<Stream()> open;               // a stream configured and ready to start; nullptr if none
        std::function<int(Stream)> start;           // AAudio results: 0 is AAUDIO_OK
        std::function<int(Stream)> pause;
        std::function<bool(Stream)> disconnected;   // its state reads AAUDIO_STREAM_STATE_DISCONNECTED
        std::function<void(Stream)> close;          // stop and close
        std::function<void(bool warning, const std::string&)> log;   // a loss or a failure is a warning
        std::function<std::string(int)> describe;   // a result's name (AAudio_convertResultToText); optional
    };
    static constexpr unsigned kLogLines = 40;       // lines a session; after that every 100th

    explicit SoundOutput(System system, Timing timing = Timing())
        : system_(std::move(system)), timing_(timing), delayMs_(timing.firstRetryMs) {}

    // The game wants sound (DirectSoundCreate): the first stream, started unless the game is paused (then at the
    // resume). False while there is none; service() then has another try due.
    bool begin(uint64_t nowMs) {
        if (begun_) return stream_ != nullptr;
        begun_ = true;
        return !paused_ && attempt(nowMs);
    }

    // The game paused or resumed. True when service() has work to do now (a stream to open).
    bool setPaused(bool paused, uint64_t nowMs) {
        if (paused == paused_) return false;
        paused_ = paused;
        if (!begun_) return false;
        if (paused) {
            endSilence(nowMs);   // time in the background is not time without sound
            takeReport(nowMs);
            if (running_) {
                running_ = false;
                if (int result = system_.pause(stream_)) lose("pause failed: " + name(result), nowMs);
            }
            return false;
        }
        takeReport(nowMs);
        if (stream_ && !running_) {
            if (system_.disconnected(stream_)) lose("disconnected while the game was paused", nowMs);
            else if (int result = system_.start(stream_)) lose("start failed: " + name(result), nowMs);
            else running_ = true;
        }
        if (stream_) return false;
        startSilence(nowMs);
        retryAtMs_ = nowMs;   // back in the game: try at once, whatever the wait was
        return true;
    }

    // AAudio's error callback: lock-free, safe on any thread. The stream is replaced by the next service() or
    // setPaused().
    void markLost(int error) noexcept {
        lostError_.store(error, std::memory_order_relaxed);
        lost_.store(true, std::memory_order_release);
    }

    // The work that is due: a reported error acted on, a disconnect AAudio did not report noticed, a stream
    // opened when the game plays without one. Returns when the next try is due (ms; 0: none pending).
    uint64_t service(uint64_t nowMs) {
        if (!begun_) return 0;
        takeReport(nowMs);
        if (running_ && system_.disconnected(stream_)) lose("disconnected", nowMs);
        if (!stream_ && !paused_ && nowMs >= retryAtMs_) attempt(nowMs);
        return !stream_ && !paused_ ? std::max<uint64_t>(retryAtMs_, 1) : 0;
    }

    // The game is done with sound: the stream closed, nothing opened again.
    void end(uint64_t nowMs) {
        endSilence(nowMs);
        closeStream();
        begun_ = false;
    }

    Stream stream() const { return stream_; }
    bool playing() const { return running_; }
    bool paused() const { return paused_; }
    // The game runs and wants sound, and no stream plays it: the time the mixer must be run without one, so the
    // game's play cursors move as on a working device (guest_audio.inc's stand-in).
    bool silent() const { return begun_ && !paused_ && !running_; }
    unsigned losses() const { return losses_; }              // streams lost this session
    unsigned replacements() const { return replacements_; }  // and replaced
    // Milliseconds the game ran (not paused) with sound wanted and no stream playing.
    uint64_t silentMs(uint64_t nowMs) const {
        return silentMs_ + (silent_ && nowMs > silentSince_ ? nowMs - silentSince_ : 0);
    }

private:
    std::string name(int result) const {
        std::string text = system_.describe ? system_.describe(result) : std::string();
        return text.empty() ? std::to_string(result) : text + " (" + std::to_string(result) + ")";
    }
    static std::string seconds(uint64_t ms) {
        if (ms < 1000) return std::to_string(ms) + " ms";
        char text[32];
        std::snprintf(text, sizeof text, "%.1f s", double(ms) / 1000.0);
        return text;
    }
    void log(bool warning, const std::string& line) {
        ++logLines_;
        if ((logLines_ <= kLogLines || logLines_ % 100 == 0) && system_.log) system_.log(warning, line);
    }
    // An error the callback reported belongs to the stream open now (closing a stream clears it).
    void takeReport(uint64_t nowMs) {
        if (!lost_.exchange(false, std::memory_order_acquire)) return;
        if (stream_) lose("AAudio reported " + name(lostError_.load(std::memory_order_relaxed)), nowMs);
    }
    void lose(const std::string& why, uint64_t nowMs) {
        bool brief = nowMs - openedAtMs_ < timing_.stableMs;
        closeStream();
        ++losses_;
        replacing_ = true;
        if (!paused_) startSilence(nowMs);
        std::string next;
        if (brief) {   // lost as soon as it was opened: the wait of a failed open
            retryAtMs_ = nowMs + delayMs_;
            next = "a new one in " + seconds(delayMs_);
            delayMs_ = std::min(delayMs_ * 2, timing_.lastRetryMs);
        } else {
            retryAtMs_ = nowMs;
            delayMs_ = timing_.firstRetryMs;
            next = "opening a new one";
        }
        log(true, "Sound output lost (" + why + "): " + (paused_ ? std::string("a new stream opens when the game resumes") : next));
    }
    void closeStream() {
        if (stream_) system_.close(stream_);
        stream_ = nullptr;
        running_ = false;
        lost_.store(false, std::memory_order_release);   // what it reported went with it
    }
    void startSilence(uint64_t nowMs) {
        if (silent_) return;
        silent_ = true;
        silentSince_ = nowMs;
    }
    // Ends a stretch without sound (a stream plays again, or the game pauses); `episodeMs_` sums the stretches
    // since a stream last played, for the line that says it plays again.
    void endSilence(uint64_t nowMs) {
        if (!silent_) return;
        uint64_t stretch = nowMs > silentSince_ ? nowMs - silentSince_ : 0;
        silentMs_ += stretch;
        episodeMs_ += stretch;
        silent_ = false;
    }
    bool attempt(uint64_t nowMs) {
        Stream stream = system_.open();
        int result = 0;
        if (stream && !paused_ && (result = system_.start(stream)) != 0) {
            system_.close(stream);
            lost_.store(false, std::memory_order_release);
            stream = nullptr;
        }
        if (!stream) {
            ++failures_;
            if (!paused_) startSilence(nowMs);
            retryAtMs_ = nowMs + delayMs_;
            log(true, "Sound output: no stream (" + (result ? "start failed: " + name(result) : std::string("open failed"))
                + "), attempt " + std::to_string(failures_) + ", the next in " + seconds(delayMs_));
            delayMs_ = std::min(delayMs_ * 2, timing_.lastRetryMs);
            return false;
        }
        stream_ = stream;
        running_ = !paused_;
        openedAtMs_ = nowMs;
        endSilence(nowMs);
        if (replacing_ || failures_) {
            if (replacing_) ++replacements_;
            log(false, std::string("Sound output ") + (replacing_ ? "replaced" : "opened") + ": a new stream after "
                + std::to_string(episodeMs_) + " ms without sound"
                + (failures_ ? " (" + std::to_string(failures_ + 1) + " attempts)" : std::string()));
        }
        episodeMs_ = 0;
        replacing_ = false;
        failures_ = 0;
        retryAtMs_ = 0;
        return true;
    }

    System system_;
    Timing timing_;
    Stream stream_ = nullptr;
    bool begun_ = false, paused_ = false, running_ = false, replacing_ = false, silent_ = false;
    std::atomic<bool> lost_{false};
    std::atomic<int> lostError_{0};
    uint64_t retryAtMs_ = 0, delayMs_, openedAtMs_ = 0, silentSince_ = 0, silentMs_ = 0, episodeMs_ = 0;
    unsigned failures_ = 0, losses_ = 0, replacements_ = 0, logLines_ = 0;
};
