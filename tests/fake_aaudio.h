#pragma once
// A simulated AAudio for tests/sound_device.cpp: the calls sound_device.h makes, with streams whose callback
// thread pulls a burst every 2 ms as AAudio's does, and the faults to test with: a stream the audio server takes
// back while it is paused (its next start answers AAUDIO_ERROR_DISCONNECTED, every later one
// AAUDIO_ERROR_INVALID_STATE), a disconnect while it plays (the error callback on the callback thread, which then
// ends), opens that fail, slow opens. As in AAudio, pausing, stopping and closing wait for the callback thread.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

typedef int32_t aaudio_result_t;
typedef int32_t aaudio_stream_state_t;
typedef int32_t aaudio_direction_t;
typedef int32_t aaudio_format_t;
typedef int32_t aaudio_sharing_mode_t;
typedef int32_t aaudio_performance_mode_t;
typedef int32_t aaudio_data_callback_result_t;
enum : int32_t { AAUDIO_OK = 0, AAUDIO_ERROR_DISCONNECTED = -899, AAUDIO_ERROR_INVALID_STATE = -895, AAUDIO_ERROR_UNAVAILABLE = -889 };
enum : int32_t { AAUDIO_DIRECTION_OUTPUT = 0 };
enum : int32_t { AAUDIO_FORMAT_PCM_FLOAT = 2 };
enum : int32_t { AAUDIO_SHARING_MODE_SHARED = 1 };
enum : int32_t { AAUDIO_PERFORMANCE_MODE_NONE = 10, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY = 12 };
enum : int32_t { AAUDIO_CALLBACK_RESULT_CONTINUE = 0 };
enum : int32_t { AAUDIO_STREAM_STATE_OPEN = 2, AAUDIO_STREAM_STATE_STARTED = 4, AAUDIO_STREAM_STATE_PAUSED = 6,
                 AAUDIO_STREAM_STATE_STOPPED = 10, AAUDIO_STREAM_STATE_DISCONNECTED = 13 };
struct AAudioStream;
typedef aaudio_data_callback_result_t (*AAudioStream_dataCallback)(AAudioStream*, void*, void*, int32_t);
typedef void (*AAudioStream_errorCallback)(AAudioStream*, void*, aaudio_result_t);

struct AAudioStream {
    AAudioStream_dataCallback data = nullptr;
    AAudioStream_errorCallback error = nullptr;
    void* dataUser = nullptr;
    void* errorUser = nullptr;
    aaudio_performance_mode_t mode = AAUDIO_PERFORMANCE_MODE_NONE;
    int32_t bufferSize = 0;
    std::mutex mutex;
    std::condition_variable changed;
    bool running = false, started = false, closed = false;
    std::atomic<bool> disconnected{false};
    std::atomic<bool> stolen{false};          // taken back while paused: the next start says DISCONNECTED
    std::atomic<bool> unplug{false};          // the test pulls the plug while it plays
    std::atomic<uint64_t> bursts{0};
    std::atomic<int32_t> xruns{0};
    std::thread thread;
};

struct AAudioStreamBuilder {
    AAudioStream_dataCallback data = nullptr;
    AAudioStream_errorCallback error = nullptr;
    void* dataUser = nullptr;
    void* errorUser = nullptr;
    aaudio_performance_mode_t mode = AAUDIO_PERFORMANCE_MODE_NONE;
};

namespace fake_aaudio {
struct World {
    std::mutex mutex;
    unsigned failOpens = 0;                   // the next opens that fail
    int openDelayMs = 0;                      // how long an open takes
    int32_t rate = 48000, burst = 96, capacity = 4800;
    std::vector<std::unique_ptr<AAudioStream>> streams;   // every stream opened, kept to the end
    std::atomic<unsigned> opened{0}, closed{0};
};
inline World& world() { static World instance; return instance; }

// The callback thread: a burst every 2 ms while running; after an unplug, the error callback once, then it ends.
inline void callbackLoop(AAudioStream* stream) {
    const int32_t burst = world().burst;
    std::vector<float> buffer(size_t(burst) * 2);
    auto period = std::chrono::microseconds(1000000LL * burst / world().rate);
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(stream->mutex);
            if (!stream->running) return;
        }
        if (stream->unplug.exchange(false)) {
            stream->disconnected = true;
            if (stream->error) stream->error(stream, stream->errorUser, AAUDIO_ERROR_DISCONNECTED);
            std::lock_guard<std::mutex> lock(stream->mutex);
            stream->running = false;
            return;
        }
        stream->data(stream, stream->dataUser, buffer.data(), burst);
        ++stream->bursts;
        next += period;
        std::unique_lock<std::mutex> lock(stream->mutex);
        stream->changed.wait_until(lock, next, [stream] { return !stream->running; });
    }
}
// Ends the callback thread and waits for it (outside the stream's lock: the thread takes it to leave).
inline void halt(AAudioStream* stream) {
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock(stream->mutex);
        stream->running = false;
        thread = std::move(stream->thread);
    }
    stream->changed.notify_all();
    if (thread.joinable()) thread.join();
}
}  // namespace fake_aaudio

inline aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder** builder) { *builder = new AAudioStreamBuilder(); return AAUDIO_OK; }
inline void AAudioStreamBuilder_setDirection(AAudioStreamBuilder*, aaudio_direction_t) {}
inline void AAudioStreamBuilder_setFormat(AAudioStreamBuilder*, aaudio_format_t) {}
inline void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder*, int32_t) {}
inline void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder*, aaudio_sharing_mode_t) {}
inline void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder* builder, aaudio_performance_mode_t mode) { builder->mode = mode; }
inline void AAudioStreamBuilder_setBufferCapacityInFrames(AAudioStreamBuilder*, int32_t) {}
inline void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder* builder, AAudioStream_dataCallback callback, void* user) {
    builder->data = callback; builder->dataUser = user;
}
inline void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder* builder, AAudioStream_errorCallback callback, void* user) {
    builder->error = callback; builder->errorUser = user;
}
inline aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder* builder, AAudioStream** out) {
    auto& world = fake_aaudio::world();
    int delay;
    {
        std::lock_guard<std::mutex> lock(world.mutex);
        delay = world.openDelayMs;
        if (world.failOpens) { --world.failOpens; *out = nullptr; return AAUDIO_ERROR_UNAVAILABLE; }
    }
    if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    auto stream = std::make_unique<AAudioStream>();
    stream->data = builder->data; stream->dataUser = builder->dataUser;
    stream->error = builder->error; stream->errorUser = builder->errorUser;
    stream->mode = builder->mode; stream->bufferSize = world.capacity;
    *out = stream.get();
    std::lock_guard<std::mutex> lock(world.mutex);
    world.streams.push_back(std::move(stream));
    ++world.opened;
    return AAUDIO_OK;
}
inline aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder* builder) { delete builder; return AAUDIO_OK; }

inline aaudio_result_t AAudioStream_requestStart(AAudioStream* stream) {
    std::lock_guard<std::mutex> lock(stream->mutex);
    if (stream->closed || stream->disconnected) return AAUDIO_ERROR_INVALID_STATE;   // "systemStart() stream is disconnected"
    if (stream->stolen) { stream->disconnected = true; return AAUDIO_ERROR_DISCONNECTED; }
    if (stream->running) return AAUDIO_OK;
    if (stream->thread.joinable()) return AAUDIO_ERROR_INVALID_STATE;   // halted but not joined: never happens here
    stream->running = stream->started = true;
    stream->thread = std::thread(fake_aaudio::callbackLoop, stream);
    return AAUDIO_OK;
}
inline aaudio_result_t AAudioStream_requestPause(AAudioStream* stream) {
    if (stream->disconnected) return AAUDIO_ERROR_DISCONNECTED;
    fake_aaudio::halt(stream);
    return AAUDIO_OK;
}
inline aaudio_result_t AAudioStream_requestStop(AAudioStream* stream) {
    fake_aaudio::halt(stream);
    return stream->disconnected ? AAUDIO_ERROR_DISCONNECTED : AAUDIO_OK;
}
inline aaudio_result_t AAudioStream_close(AAudioStream* stream) {
    fake_aaudio::halt(stream);
    std::lock_guard<std::mutex> lock(stream->mutex);
    if (!stream->closed) { stream->closed = true; ++fake_aaudio::world().closed; }
    return AAUDIO_OK;
}
inline aaudio_stream_state_t AAudioStream_getState(AAudioStream* stream) {
    if (stream->disconnected) return AAUDIO_STREAM_STATE_DISCONNECTED;
    std::lock_guard<std::mutex> lock(stream->mutex);
    return stream->running ? AAUDIO_STREAM_STATE_STARTED : stream->started ? AAUDIO_STREAM_STATE_PAUSED : AAUDIO_STREAM_STATE_OPEN;
}
inline int32_t AAudioStream_getSampleRate(AAudioStream*) { return fake_aaudio::world().rate; }
inline int32_t AAudioStream_getFramesPerBurst(AAudioStream*) { return fake_aaudio::world().burst; }
inline int32_t AAudioStream_getBufferCapacityInFrames(AAudioStream*) { return fake_aaudio::world().capacity; }
inline int32_t AAudioStream_getBufferSizeInFrames(AAudioStream* stream) { std::lock_guard<std::mutex> lock(stream->mutex); return stream->bufferSize; }
inline aaudio_result_t AAudioStream_setBufferSizeInFrames(AAudioStream* stream, int32_t frames) {
    std::lock_guard<std::mutex> lock(stream->mutex);
    stream->bufferSize = std::max(1, std::min(frames, fake_aaudio::world().capacity));
    return stream->bufferSize;
}
inline int32_t AAudioStream_getXRunCount(AAudioStream* stream) { return stream->xruns.load(); }
inline aaudio_performance_mode_t AAudioStream_getPerformanceMode(AAudioStream* stream) { return stream->mode; }
inline const char* AAudio_convertResultToText(aaudio_result_t result) {
    switch (result) {
        case AAUDIO_OK: return "AAUDIO_OK";
        case AAUDIO_ERROR_DISCONNECTED: return "AAUDIO_ERROR_DISCONNECTED";
        case AAUDIO_ERROR_INVALID_STATE: return "AAUDIO_ERROR_INVALID_STATE";
        case AAUDIO_ERROR_UNAVAILABLE: return "AAUDIO_ERROR_UNAVAILABLE";
        default: return "AAUDIO_ERROR_UNKNOWN";
    }
}
