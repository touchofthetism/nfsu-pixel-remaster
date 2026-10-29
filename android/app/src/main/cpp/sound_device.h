#pragma once
// The game's sound output on Android (guest_audio.inc): the guest's mixer (audio_mixer.h) through the peak limiter
// (audio_limiter.h) into one AAudio stream at a time, kept by SoundOutput (sound_output.h) and by a keeper thread
// that replaces lost streams and stands in for one while there is none. tests/sound_device.cpp runs this file
// against a simulated AAudio (tests/fake_aaudio.h) under ThreadSanitizer and AddressSanitizer.
//
// Threads and locks:
//  - lock_ guards the stream and every counter of it: the main thread pauses and resumes (setPaused), the keeper
//    replaces streams and stands in, the game thread looks after the buffer (service: a try-lock, so a
//    replacement in progress never holds a frame up), the session recorder and the overlay read the figures
//    (report: a try-lock too, the overlay asks from the UI thread). It is never taken on AAudio's threads:
//    closing a stream waits for its callback thread, which may be in errorCallback.
//  - wakeLock_ is held only to set or read the keeper's wake request.
//  - the data callback takes only the mixer's own lock.
#include "audio_limiter.h"
#include "audio_mixer.h"
#include "runtime_log.h"
#include "sound_output.h"
#ifndef NFS_FAKE_AAUDIO   // tests include their simulated AAudio first
#include <aaudio/AAudio.h>
#endif
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class SoundDevice {
public:
    // Output buffer: 40 ms, grown to 100 ms after reported underruns. Two bursts (4-10 ms on MediaTek, Exynos and
    // Unisoc low-latency streams) were all a render or guest-lock hiccup needed to break the sound on busy low-end
    // phones, and those drivers report no xruns, so the old grow-on-xrun rule never fired. A racing game does not
    // need lower latency than this.
    static constexpr int32_t kTargetMs = 40, kMaximumMs = 100;
    // The stand-in mixes every 10 ms, at most 100 ms at once (a keeper held up longer skips ahead).
    static constexpr int kStandInMs = 10, kStandInMostMs = 100;

    explicit SoundDevice(PcmMixer& mixer, SoundOutput::Timing timing = SoundOutput::Timing())
        : mixer_(mixer), output_(system(), timing) {}
    ~SoundDevice() { stop(); }
    SoundDevice(const SoundDevice&) = delete;
    SoundDevice& operator=(const SoundDevice&) = delete;

    // The game asked for sound (DirectSoundCreate). Its device works either way: without a stream the keeper
    // stands in until one opens.
    void start() {
        {
            std::lock_guard<std::mutex> lock(lock_);
            if (!output_.begin(nowMs()))
                NFS_RUNTIME_LOG(ANDROID_LOG_WARN, "NFSU1", "Sound: no stream yet; the game runs on and sound starts when one opens");
        }
        {
            std::lock_guard<std::mutex> lock(wakeLock_);
            if (!keeper_.joinable()) { stopping_ = false; keeper_ = std::thread([this] { runKeeper(); }); }
        }
        wake();
    }

    // The game paused or resumed (the main thread): the stream follows; one the audio server took while the app
    // was away is replaced by the keeper.
    void setPaused(bool paused) {
        bool work;
        { std::lock_guard<std::mutex> lock(lock_); work = output_.setPaused(paused, nowMs()); }
        if (work) wake();
    }

    // The game is done with sound: the keeper stopped, the stream closed (its counters kept for the record).
    void stop() {
        { std::lock_guard<std::mutex> lock(wakeLock_); stopping_ = true; }
        wakeup_.notify_one();
        if (keeper_.joinable()) keeper_.join();
        std::lock_guard<std::mutex> lock(lock_);
        output_.end(nowMs());
    }

    // What the session record shows. A replacement in progress is never waited for: the last figures are given.
    struct Report { int32_t xruns = -1, bufferFrames = 0, burst = 0; unsigned rate = 0, lost = 0, replaced = 0; uint64_t silentMs = 0; };
    Report report() {
        std::unique_lock<std::mutex> lock(lock_, std::try_to_lock);
        if (!lock.owns_lock()) { std::lock_guard<std::mutex> last(reportLock_); return lastReport_; }
        Report report;
        report.xruns = closed_.xruns; report.bufferFrames = closed_.bufferFrames; report.burst = closed_.burst; report.rate = closed_.rate;
        if (AAudioStream* stream = current()) {
            int32_t xruns = AAudioStream_getXRunCount(stream);
            report.xruns = std::max(closed_.xruns, 0) + std::max(xruns, 0);
            report.bufferFrames = AAudioStream_getBufferSizeInFrames(stream);
            report.burst = AAudioStream_getFramesPerBurst(stream);
            report.rate = mixer_.rate;
        }
        report.lost = output_.losses(); report.replaced = output_.replacements(); report.silentMs = output_.silentMs(nowMs());
        std::lock_guard<std::mutex> last(reportLock_);
        lastReport_ = report;
        return report;
    }

    // The game thread, every 120 frames: a buffer one burst larger after new underruns (at most 100 ms), and the
    // stream's health for the log. False while there is no stream, or one is being replaced.
    struct Health { int32_t xruns = 0, bufferFrames = 0, capacity = 0, burst = 0, mode = 0; unsigned rate = 0, lost = 0; uint32_t callbackPeakUs = 0; uint64_t silentMs = 0; };
    bool service(Health* health) {
        std::unique_lock<std::mutex> lock(lock_, std::try_to_lock);
        AAudioStream* stream = lock.owns_lock() ? current() : nullptr;
        if (!stream) return false;
        int32_t xruns = AAudioStream_getXRunCount(stream), burst = AAudioStream_getFramesPerBurst(stream);
        int32_t size = AAudioStream_getBufferSizeInFrames(stream), most = frames(stream, kMaximumMs);
        if (xruns > lastXruns_ && burst > 0 && size < most) AAudioStream_setBufferSizeInFrames(stream, std::min(size + burst, most));
        if (xruns >= 0) lastXruns_ = xruns;
        health->xruns = xruns; health->bufferFrames = AAudioStream_getBufferSizeInFrames(stream);
        health->capacity = AAudioStream_getBufferCapacityInFrames(stream); health->burst = burst;
        health->mode = int(AAudioStream_getPerformanceMode(stream)); health->rate = mixer_.rate;
        health->callbackPeakUs = callbackPeakUs_.exchange(0, std::memory_order_relaxed);
        health->lost = output_.losses(); health->silentMs = output_.silentMs(nowMs());
        return true;
    }
    // Frames the limiter brought down since the last call.
    unsigned long long limitedFrames() { return static_cast<unsigned long long>(limiter_.limitedFrames.exchange(0, std::memory_order_relaxed)); }
    // Frames the stand-in has mixed in all (tests).
    uint64_t standInFrames() { std::lock_guard<std::mutex> lock(lock_); return standInFrames_; }

private:
    static uint64_t nowMs() {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    AAudioStream* current() const { return static_cast<AAudioStream*>(output_.stream()); }

    // The mix can sum past full scale (music + engine + effects in a race); the look-ahead limiter keeps it in range
    // without flattening the peaks. Configured when a stream opens, then used only on its callback thread.
    static aaudio_data_callback_result_t dataCallback(AAudioStream*, void* user, void* data, int32_t frames) {
        auto& self = *static_cast<SoundDevice*>(user);
        auto start = std::chrono::steady_clock::now();
        self.mixer_.render(static_cast<float*>(data), unsigned(frames));
        self.limiter_.process(static_cast<float*>(data), unsigned(frames));
        uint32_t us = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        auto peak = self.callbackPeakUs_.load(std::memory_order_relaxed);
        while (us > peak && !self.callbackPeakUs_.compare_exchange_weak(peak, us, std::memory_order_relaxed)) {}
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    // On AAudio's thread: only a flag and a wake (sound_output.h).
    static void errorCallback(AAudioStream*, void* user, aaudio_result_t error) {
        auto& self = *static_cast<SoundDevice*>(user);
        self.output_.markLost(int(error));
        self.wake();
    }
    AAudioStream* openStream(aaudio_performance_mode_t mode) {
        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return nullptr;
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(builder, mode);
        AAudioStreamBuilder_setBufferCapacityInFrames(builder, 48000 * kMaximumMs / 1000);
        AAudioStreamBuilder_setDataCallback(builder, dataCallback, this);
        AAudioStreamBuilder_setErrorCallback(builder, errorCallback, this);
        AAudioStream* stream = nullptr;
        auto status = AAudioStreamBuilder_openStream(builder, &stream);
        AAudioStreamBuilder_delete(builder);
        return status == AAUDIO_OK ? stream : nullptr;
    }
    int32_t frames(AAudioStream* stream, int32_t milliseconds) const {
        int32_t count = int32_t(mixer_.rate) * milliseconds / 1000, burst = AAudioStream_getFramesPerBurst(stream);
        if (burst > 0) count = (count + burst - 1) / burst * burst;
        return std::min(count, AAudioStream_getBufferCapacityInFrames(stream));
    }
    // A stream for the game, configured and not started: low latency, unless that one's buffer cannot hold 40 ms
    // (some MediaTek low-latency streams stop at two bursts, 512 frames), then the default mode.
    AAudioStream* openGameStream() {
        AAudioStream* stream = openStream(AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        if (stream && AAudioStream_getBufferCapacityInFrames(stream) < AAudioStream_getSampleRate(stream) * kTargetMs / 1000) {
            if (AAudioStream* fallback = openStream(AAUDIO_PERFORMANCE_MODE_NONE)) { AAudioStream_close(stream); stream = fallback; }
        }
        if (!stream) stream = openStream(AAUDIO_PERFORMANCE_MODE_NONE);
        if (!stream) return nullptr;
        // No callback runs now (the stream before this one is closed): the mixer's rate and the limiter may change.
        { std::lock_guard<std::mutex> lock(mixer_.mutex); mixer_.rate = unsigned(AAudioStream_getSampleRate(stream)); }
        limiter_.configure(mixer_.rate);
        AAudioStream_setBufferSizeInFrames(stream, frames(stream, kTargetMs));
        lastXruns_ = 0;
        callbackPeakUs_ = 0;
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO, "NFSU1", "AAudio stereo float stream opened: %u Hz, %s, burst %d", mixer_.rate,
                        AAudioStream_getPerformanceMode(stream) == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? "low latency" : "default mode",
                        AAudioStream_getFramesPerBurst(stream));
        return stream;
    }
    void closeGameStream(AAudioStream* stream) {
        int32_t xruns = AAudioStream_getXRunCount(stream);
        closed_ = {std::max(closed_.xruns, 0) + std::max(xruns, 0), AAudioStream_getBufferSizeInFrames(stream),
                   AAudioStream_getFramesPerBurst(stream), mixer_.rate};
        AAudioStream_requestStop(stream);
        AAudioStream_close(stream);
    }
    SoundOutput::System system() {
        SoundOutput::System system;
        auto stream = [](SoundOutput::Stream handle) { return static_cast<AAudioStream*>(handle); };
        system.open = [this] { return static_cast<SoundOutput::Stream>(openGameStream()); };
        system.start = [stream](SoundOutput::Stream handle) { return int(AAudioStream_requestStart(stream(handle))); };
        system.pause = [stream](SoundOutput::Stream handle) { return int(AAudioStream_requestPause(stream(handle))); };
        system.disconnected = [stream](SoundOutput::Stream handle) { return AAudioStream_getState(stream(handle)) == AAUDIO_STREAM_STATE_DISCONNECTED; };
        system.close = [this, stream](SoundOutput::Stream handle) { closeGameStream(stream(handle)); };
        system.log = [](bool warning, const std::string& line) {
            NFS_RUNTIME_LOG(warning ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "NFSU1", "%s", line.c_str());
        };
        system.describe = [](int result) {
            const char* text = AAudio_convertResultToText(aaudio_result_t(result));
            return std::string(text ? text : "");
        };
        return system;
    }

    void wake() {
        { std::lock_guard<std::mutex> lock(wakeLock_); wakeRequested_ = true; }
        wakeup_.notify_one();
    }
    // The keeper: opens the replacement for a lost stream away from the game's threads (an open and a start take
    // 50-110 ms on the Pixel), retries failed opens when they are due, and checks once a second that the playing
    // stream still works. While the game runs with no stream playing, it also stands in for one: every 10 ms it
    // runs the mixer for the time gone by into a buffer nobody hears, so the play cursors of the game's DirectSound
    // buffers move as on a working device (a game waiting on a cursor never waits for good) and the music and the
    // engine go on from where the game is when a stream plays again. It never mixes while a stream plays: both
    // happen under lock_, and once the keeper runs only it opens streams.
    void runKeeper() {
        pthread_setname_np(pthread_self(), "nfs-sound");
        uint64_t due = 0;
        bool standIn = false;
        std::vector<float> scratch;
        std::chrono::steady_clock::time_point mixedTo{};   // how far the stand-in has mixed
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(wakeLock_);
                uint64_t now = nowMs(), wait = due ? (due > now ? std::min<uint64_t>(due - now, 1000) : 0) : 1000;
                if (standIn) wait = std::min<uint64_t>(wait, kStandInMs);
                wakeup_.wait_for(lock, std::chrono::milliseconds(wait), [this] { return wakeRequested_ || stopping_; });
                if (stopping_) return;
                wakeRequested_ = false;
            }
            std::lock_guard<std::mutex> lock(lock_);
            due = output_.service(nowMs());
            standIn = output_.silent();
            if (!standIn) { mixedTo = {}; continue; }
            auto now = std::chrono::steady_clock::now();
            if (mixedTo == std::chrono::steady_clock::time_point{}) mixedTo = now;   // a stretch without a stream begins
            if (now - mixedTo > std::chrono::milliseconds(kStandInMostMs)) mixedTo = now - std::chrono::milliseconds(kStandInMostMs);
            uint64_t rate = mixer_.rate ? mixer_.rate : 48000;
            uint64_t count = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - mixedTo).count()) * rate / 1000000000ull;
            if (!count) continue;
            scratch.resize(size_t(count) * 2);
            mixer_.render(scratch.data(), unsigned(count));
            standInFrames_ += count;
            mixedTo += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(count * 1000000000ull / rate));
        }
    }

    PcmMixer& mixer_;
    PeakLimiter limiter_;
    std::mutex lock_;
    SoundOutput output_;
    struct Closed { int32_t xruns = -1, bufferFrames = 0, burst = 0; unsigned rate = 0; } closed_;   // the last stream closed
    int32_t lastXruns_ = 0;
    std::atomic<uint32_t> callbackPeakUs_{0};
    std::mutex reportLock_;
    Report lastReport_;
    std::mutex wakeLock_;
    std::condition_variable wakeup_;
    bool wakeRequested_ = false, stopping_ = false;
    std::thread keeper_;
    uint64_t standInFrames_ = 0;
};
