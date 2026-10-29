// The sound output (android/app/src/main/cpp/sound_output.h) against a simulated AAudio:
//  - pause and resume pause and start the one stream, and nothing is replaced while it works;
//  - build 14's loss: a stream the audio server took back while the game was in the background answers the
//    resume with AAUDIO_ERROR_DISCONNECTED; it is closed once, never started again (no INVALID_STATE answers),
//    and a new stream plays on the next service();
//  - a disconnect while playing (the error callback), one AAudio did not report (the state), one reported while
//    the game pauses, a failed pause: each closes the stream, and the new one waits for the resume when paused;
//  - failed opens and failed starts are tried again after 0.5, 1, 2, 4, 8, 8 s; a resume tries at once;
//  - a device that drops every stream at once gets the same waits, a stream that lasted starts them over;
//  - a report from a stream already closed never takes down its successor;
//  - the time without sound counts only while the game runs; the counters, the log lines and their limit;
//  - markLost from another thread while the owner services (run under TSan in CI's sanitizer pass if any).
#include "../android/app/src/main/cpp/sound_output.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

static void require(bool ok, const std::string& message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message.c_str()); std::exit(1); }
}

namespace {
constexpr int kDisconnected = -899, kInvalidState = -895, kUnavailable = -889;

struct FakeStream {
    unsigned id = 0, starts = 0, pauses = 0, closes = 0;
    bool started = false;
    bool stolen = false;        // taken back while paused: the next start says DISCONNECTED, then it is disconnected
    bool disconnected = false;  // what AAudioStream_getState reports
    int startResult = 0, pauseResult = 0;
    bool reportOnClose = false; // its callback thread reports an error while close() is ending it
};

std::vector<std::unique_ptr<FakeStream>> streams;
unsigned failOpens = 0;        // the next opens that fail
int firstStartResult = 0;      // the next opened stream's first start answers this
std::vector<std::string> lines;
SoundOutput* reportTo = nullptr;   // the output a reportOnClose stream reports to

FakeStream* fake(SoundOutput::Stream stream) { return static_cast<FakeStream*>(stream); }

SoundOutput::System simulated() {
    SoundOutput::System system;
    system.open = []() -> SoundOutput::Stream {
        if (failOpens) { --failOpens; return nullptr; }
        streams.push_back(std::make_unique<FakeStream>());
        FakeStream* stream = streams.back().get();
        stream->id = unsigned(streams.size());
        stream->startResult = firstStartResult;
        firstStartResult = 0;
        return stream;
    };
    system.start = [](SoundOutput::Stream handle) {
        FakeStream* stream = fake(handle);
        require(!stream->closes, "start on a closed stream");
        ++stream->starts;
        if (stream->disconnected) return kInvalidState;   // "systemStart() stream is disconnected"
        if (stream->stolen) { stream->disconnected = true; return kDisconnected; }
        if (int result = stream->startResult) { stream->startResult = 0; return result; }
        stream->started = true;
        return 0;
    };
    system.pause = [](SoundOutput::Stream handle) {
        FakeStream* stream = fake(handle);
        require(!stream->closes, "pause on a closed stream");
        ++stream->pauses;
        if (stream->disconnected) return kDisconnected;
        if (stream->pauseResult) return stream->pauseResult;
        stream->started = false;
        return 0;
    };
    system.disconnected = [](SoundOutput::Stream handle) { return fake(handle)->disconnected; };
    system.close = [](SoundOutput::Stream handle) {
        FakeStream* stream = fake(handle);
        require(!stream->closes, "a stream closed twice");
        ++stream->closes;
        stream->started = false;
        if (stream->reportOnClose && reportTo) reportTo->markLost(kDisconnected);
    };
    system.log = [](bool warning, const std::string& line) {
        bool problem = line.find("lost") != std::string::npos || line.find("no stream") != std::string::npos;
        require(warning == problem, "a loss or a failure is a warning, nothing else: " + line);
        lines.push_back(line);
    };
    system.describe = [](int result) {
        return result == kDisconnected ? std::string("AAUDIO_ERROR_DISCONNECTED")
             : result == kInvalidState ? std::string("AAUDIO_ERROR_INVALID_STATE") : std::string();
    };
    return system;
}

void reset() { streams.clear(); failOpens = 0; firstStartResult = 0; lines.clear(); }
bool logged(const std::string& part) {
    for (const auto& line : lines) if (line.find(part) != std::string::npos) return true;
    return false;
}
FakeStream* current(const SoundOutput& output) { return fake(output.stream()); }
}  // namespace

int main() {
    // A stream that works: one open, the pauses and resumes on it, no loss.
    {
        reset();
        SoundOutput output(simulated());
        require(output.begin(1000) && streams.size() == 1 && current(output)->started && output.playing(), "the first stream plays");
        require(!output.setPaused(true, 2000) && !current(output)->started && current(output)->pauses == 1, "paused with the game");
        require(!output.setPaused(true, 2100) && current(output)->pauses == 1, "a second pause does nothing");
        require(!output.setPaused(false, 3000) && current(output)->started && current(output)->starts == 2, "started on resume");
        for (uint64_t t = 3000; t < 60000; t += 1000) require(output.service(t) == 0, "nothing due while it works");
        require(streams.size() == 1 && output.losses() == 0 && output.silentMs(60000) == 0 && lines.empty(), "no replacement, no line");
        output.end(61000);
        require(current(output) == nullptr && streams[0]->closes == 1 && output.service(62000) == 0, "end closes it, and stays closed");
    }

    // Build 14: taken back in the background. The resume closes it and asks for service; the new stream plays.
    {
        reset();
        SoundOutput output(simulated());
        output.begin(0);
        output.setPaused(true, 10000);
        streams[0]->stolen = true;
        require(output.setPaused(false, 50000), "a resume that finds the stream gone asks for service");
        require(streams[0]->starts == 2 && streams[0]->closes == 1 && output.stream() == nullptr, "closed after one failed start");
        require(output.silent(), "silent until the new stream plays: the stand-in mixes meanwhile");
        require(logged("Sound output lost (start failed: AAUDIO_ERROR_DISCONNECTED (-899)): opening a new one"), lines.empty() ? "no line" : lines.back());
        // The window commands pause and resume again before the keeper runs: no more starts on the dead stream.
        require(!output.setPaused(true, 50010) && output.setPaused(false, 50020) && streams[0]->starts == 2, "no INVALID_STATE answers");
        require(output.service(50085) == 0 && streams.size() == 2 && current(output)->started && output.playing() && !output.silent(), "a new stream plays");
        require(output.losses() == 1 && output.replacements() == 1, "one loss, one replacement");
        require(output.silentMs(50085) == 75, "75 ms without sound, the background and the 10 ms window pause not counted: "
                + std::to_string(output.silentMs(50085)));
        require(logged("Sound output replaced: a new stream after 75 ms without sound"), lines.back());
    }

    // Disconnected while playing (headphones out): the error callback, then the keeper's service().
    {
        reset();
        SoundOutput output(simulated());
        output.begin(0);
        streams[0]->disconnected = true;
        output.markLost(kDisconnected);
        require(output.service(20000) == 0 && streams[0]->closes == 1 && streams.size() == 2 && current(output)->started,
                "replaced on the next service");
        require(logged("Sound output lost (AAudio reported AAUDIO_ERROR_DISCONNECTED (-899)): opening a new one"), lines.front());
        require(output.silentMs(20000) == 0 && output.replacements() == 1, "replaced in the same service");
    }

    // A disconnect AAudio never reported: the state says it, service() notices.
    {
        reset();
        SoundOutput output(simulated());
        output.begin(0);
        require(output.service(1000) == 0 && streams.size() == 1, "nothing to do");
        streams[0]->disconnected = true;
        require(output.service(9000) == 0 && streams.size() == 2 && current(output)->started && logged("(disconnected): opening"), "noticed");
    }

    // Reported, then the game pauses before service() ran: closed at the pause, reopened at the resume only.
    {
        reset();
        SoundOutput output(simulated());
        output.begin(0);
        output.markLost(kDisconnected);
        require(!output.setPaused(true, 10000) && streams[0]->closes == 1 && streams[0]->pauses == 0, "closed, not paused");
        require(logged("a new stream opens when the game resumes"), lines.back());
        for (uint64_t t = 10000; t < 30000; t += 500) require(output.service(t) == 0, "nothing opened while paused");
        require(!output.silent(), "paused: nothing to mix");
        require(streams.size() == 1, "still one stream");
        require(output.setPaused(false, 30000) && output.service(30000) == 0 && streams.size() == 2 && current(output)->started, "at the resume");
        require(output.silentMs(30000) == 0, "no time without sound counted while paused");
    }

    // While paused: a stream that reported an error, or whose state turned disconnected, is not started at the
    // resume (no INVALID_STATE answer); it is closed and replaced.
    {
        reset();
        SoundOutput output(simulated());
        output.begin(0);
        output.setPaused(true, 10000);
        streams[0]->disconnected = true;
        output.markLost(kDisconnected);
        require(output.setPaused(false, 20000) && streams[0]->starts == 1 && streams[0]->closes == 1, "reported: not started");
        require(logged("Sound output lost (AAudio reported AAUDIO_ERROR_DISCONNECTED (-899))"), lines.back());
        require(output.service(20000) == 0 && streams.size() == 2 && current(output)->started, "replaced");
        output.setPaused(true, 30000);
        streams[1]->disconnected = true;
        require(output.setPaused(false, 40000) && streams[1]->starts == 1 && streams[1]->closes == 1, "disconnected: not started");
        require(logged("Sound output lost (disconnected while the game was paused)"), lines.back());
        require(output.service(40000) == 0 && streams.size() == 3 && current(output)->started, "replaced again");
    }

    // A pause that fails (the stream died while playing and nothing said so): closed; the resume replaces it.
    {
        reset();
        SoundOutput output(simulated());
        output.begin(0);
        streams[0]->pauseResult = kDisconnected;
        require(!output.setPaused(true, 10000) && streams[0]->closes == 1 && logged("pause failed: AAUDIO_ERROR_DISCONNECTED"), "failed pause closes");
        require(output.setPaused(false, 20000) && output.service(20001) == 0 && current(output)->started, "replaced at the resume");
    }

    // Opens that fail: tried again after 0.5, 1, 2, 4, 8, 8 s; the resume tries at once; success ends the waits.
    {
        reset();
        SoundOutput output(simulated());
        failOpens = 7;
        require(!output.begin(0) && output.stream() == nullptr && output.silent(), "no stream at first: silent");
        uint64_t due = output.service(0);
        require(due == 500, "first retry at 0.5 s: " + std::to_string(due));
        require(output.service(499) == 500 && failOpens == 6, "not before it is due");
        const uint64_t expected[] = {1500, 3500, 7500, 15500, 23500};
        uint64_t at = 500;
        for (uint64_t next : expected) {
            due = output.service(at);
            require(due == next, "retry at " + std::to_string(next) + ": " + std::to_string(due));
            at = due;
        }
        require(failOpens == 1, "six failed tries so far");
        // Paused and resumed before the next try is due (23.5 s): a try at the resume, the last failure; the waits go on.
        output.setPaused(true, 16000);
        require(output.service(17000) == 0, "no tries while paused");
        require(output.setPaused(false, 20000) && output.service(20000) == 28000 && failOpens == 0, "a try at the resume");
        require(output.service(28000) == 0 && current(output) && current(output)->started, "the eighth open works");
        require(logged("Sound output opened: a new stream after 24000 ms without sound (8 attempts)"), lines.back());
        require(output.silentMs(28000) == 28000 - 4000, "without sound while running, not while paused: " + std::to_string(output.silentMs(28000)));
        require(logged("Sound output: no stream (open failed), attempt 1, the next in 500 ms"), lines.front());
        require(output.losses() == 0 && output.replacements() == 0, "nothing was lost");
    }

    // A stream that opens but will not start: closed, and waited for like a failed open.
    {
        reset();
        SoundOutput output(simulated());
        firstStartResult = kUnavailable;
        require(!output.begin(0) && streams.size() == 1 && streams[0]->closes == 1, "closed after the failed start");
        require(output.service(0) == 500 && output.service(500) == 0 && streams.size() == 2 && current(output)->started, "the next one plays");
        require(logged("start failed: -889"), lines.front());
    }

    // A device that drops every stream at once: the waits grow; a stream that lasts 5 s starts them over.
    {
        reset();
        SoundOutput output(simulated());
        output.begin(0);
        uint64_t t = 1000;
        const uint64_t waits[] = {500, 1000, 2000, 4000};
        for (uint64_t wait : waits) {
            current(output)->disconnected = true;
            output.markLost(kDisconnected);
            uint64_t due = output.service(t);
            require(due == t + wait, "dropped at once: the next in " + std::to_string(wait) + " ms, not " + std::to_string(due - t));
            t = due;
            require(output.service(t) == 0 && current(output)->started, "reopened when due");
            t += 100;
        }
        t += 10000;   // this one lasts
        current(output)->disconnected = true;
        output.markLost(kDisconnected);
        require(output.service(t) == 0 && current(output)->started && output.losses() == 5 && output.replacements() == 5, "at once again");
    }

    // A report its stream raised while being closed (AAudio's callback thread ends inside close()) goes with it:
    // the stream that replaces it plays on. One that comes while no stream is open is ignored.
    {
        reset();
        SoundOutput output(simulated());
        reportTo = &output;
        output.begin(1000);
        streams[0]->reportOnClose = true;
        streams[0]->disconnected = true;
        require(output.service(10000) == 0 && streams.size() == 2 && current(output)->started, "replaced");
        require(output.service(11000) == 0 && current(output) == streams[1].get() && output.losses() == 1, "the late report took nothing down");
        streams[1]->pauseResult = kDisconnected;
        output.setPaused(true, 20000);                // closed for the failed pause
        output.markLost(kDisconnected);               // a report with no stream open
        require(output.setPaused(false, 30000) && output.service(30000) == 0 && streams.size() == 3 && current(output)->started, "reopened");
        require(output.service(31000) == 0 && streams.size() == 3 && output.losses() == 2, "and the stray report ignored");
        reportTo = nullptr;
    }

    // An open and a start take time (60 ms here, 95 ms on the Pixel): it counts as time without sound, and the next
    // try after a failed one is timed from when it ended.
    {
        reset();
        uint64_t clock = 1000;
        SoundOutput::System system = simulated();
        auto open = system.open;
        system.open = [&clock, open] { clock += 60; return open(); };
        system.now = [&clock] { return clock; };
        SoundOutput output(system);
        output.begin(clock);
        require(clock == 1060 && current(output)->started, "the first stream, opened in 60 ms");
        clock = 10000;
        output.setPaused(true, clock);
        streams[0]->stolen = true;
        clock = 20000;
        require(output.setPaused(false, clock) && output.service(clock) == 0 && current(output)->started && clock == 20060, "replaced");
        require(output.silentMs(clock) == 60, "the open counted: " + std::to_string(output.silentMs(clock)));
        require(logged("Sound output replaced: a new stream after 60 ms without sound"), lines.back());
        failOpens = 1;
        current(output)->disconnected = true;
        clock = 40000;
        uint64_t due = output.service(clock);
        require(due == 40060 + 500, "a failed open's wait starts when it ended: " + std::to_string(due));
    }

    // Before begin(): pauses are remembered, nothing is opened; begin() while paused opens at the resume.
    {
        reset();
        SoundOutput output(simulated());
        require(!output.silent(), "before begin(): no sound wanted");
        output.setPaused(true, 0);
        require(!output.begin(100) && streams.empty() && output.service(200) == 0, "begin while paused opens nothing");
        require(output.setPaused(false, 300) && output.service(300) == 0 && streams.size() == 1 && current(output)->started, "the resume opens it");
        require(lines.empty() && output.silentMs(300) == 0, "a first stream is no event");
    }

    // The log keeps to 40 lines a session, then every 100th.
    {
        reset();
        SoundOutput output(simulated());
        output.begin(0);
        uint64_t t = 0;
        for (unsigned i = 0; i < 150; ++i) {   // 150 losses at least 5 s apart: 300 lines (lost, replaced)
            t += 6000;
            current(output)->disconnected = true;
            output.markLost(kDisconnected);
            output.service(t);
        }
        require(output.losses() == 150 && output.replacements() == 150, "every loss replaced");
        require(lines.size() == SoundOutput::kLogLines + 3, "40 lines, then the 100th, 200th and 300th: " + std::to_string(lines.size()));
    }

    // markLost from AAudio's thread while the owner services under its lock.
    {
        reset();
        SoundOutput output(simulated());
        std::mutex lock;
        { std::lock_guard<std::mutex> hold(lock); output.begin(0); }
        std::atomic<bool> stop{false};
        std::atomic<unsigned> reports{0};
        std::thread reporter([&] {
            while (!stop.load()) { output.markLost(kDisconnected); reports.fetch_add(1); std::this_thread::yield(); }
        });
        uint64_t t = 0;
        for (unsigned i = 0; i < 2000; ++i) {
            std::lock_guard<std::mutex> hold(lock);
            t += 6000;
            output.service(t);
        }
        stop = true;
        reporter.join();
        std::lock_guard<std::mutex> hold(lock);
        output.service(t + 6000);
        output.service(t + 12000);
        require(current(output) && current(output)->started, "a stream plays at the end");
        require(output.losses() <= reports.load() && output.losses() == output.replacements(), "every loss replaced, none invented");
        for (size_t i = 0; i + 1 < streams.size(); ++i) require(streams[i]->closes == 1, "every replaced stream closed once");
    }

    std::puts("sound_output: ok");
    return 0;
}
