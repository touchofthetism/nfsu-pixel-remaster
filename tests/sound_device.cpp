// sound_device.h end to end: its keeper thread, locks and stand-in, on a simulated AAudio (fake_aaudio.h) whose
// streams run callback threads of their own that pull the mix, with the waits shortened (first retry 50 ms, at most
// 400 ms, a stream lasting 300 ms counts as stable):
//  - the first stream plays the mix; one unplugged while playing (the error callback on its own thread) is
//    replaced by the keeper and the new one plays on;
//  - paused, nothing pulls the mix; a stream taken back in the background is replaced at the resume, and the resume
//    itself does not wait for the slow open;
//  - while a replacement opens, neither the session figures nor the game thread's service wait for it;
//  - with no stream for a while (opens failing), the stand-in mixes in real time, so the game's play cursors move;
//  - underruns grow the buffer one burst at a time; stop and the destructor end the keeper and close every stream.
// ci/host_tests.sh runs it plainly; run it under -fsanitize=thread and -fsanitize=address,undefined too.
#include "fake_aaudio.h"
#define NFS_FAKE_AAUDIO 1
#include "../android/app/src/main/cpp/sound_device.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace std::chrono;

// A failure ends the process on the spot: the keeper and the streams' threads are still running then.
static void require(bool ok, const std::string& message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message.c_str()); std::fflush(stderr); std::_Exit(1); }
}

namespace {
template <class Condition> bool eventually(Condition condition, int ms = 3000) {
    auto until = steady_clock::now() + milliseconds(ms);
    while (steady_clock::now() < until) {
        if (condition()) return true;
        std::this_thread::sleep_for(milliseconds(2));
    }
    return condition();
}
AAudioStream* stream(size_t index) {
    auto& world = fake_aaudio::world();
    std::lock_guard<std::mutex> lock(world.mutex);
    return index < world.streams.size() ? world.streams[index].get() : nullptr;
}
bool playing(size_t index) {
    AAudioStream* s = stream(index);
    if (!s) return false;
    std::lock_guard<std::mutex> lock(s->mutex);
    return s->running;
}
bool closed(size_t index) {
    AAudioStream* s = stream(index);
    if (!s) return false;
    std::lock_guard<std::mutex> lock(s->mutex);
    return s->closed;
}
}  // namespace

int main() {
    auto& world = fake_aaudio::world();
    PcmMixer mixer;
    // A looping 30 s ring of silence at the stream's rate: its position counts the frames mixed.
    auto voice = std::make_shared<PcmVoice>();
    voice->channels = 2; voice->bits = 16; voice->rate = voice->frequency = 48000;
    voice->samples.assign(size_t(48000) * 30 * 4, 0);
    voice->loop = true; voice->playing = true;
    require(mixer.add(voice), "voice added");
    auto position = [&] { std::lock_guard<std::mutex> lock(mixer.mutex); return voice->position; };
    const SoundOutputTiming fast{50, 400, 300};
    SoundDevice::Health health;
    {
        SoundDevice device(mixer, fast);
        // report() answers with the last figures while the keeper holds the lock (by design: the overlay asks from
        // the UI thread), so a check of fresh figures asks until they come.
        SoundDevice::Report report;
        auto reported = [&](auto condition) { return eventually([&] { report = device.report(); return condition(report); }); };

        // 1. The first stream plays the mix.
        device.start();
        require(world.opened == 1 && playing(0), "the first stream plays");
        double before = position();
        require(eventually([&] { return position() > before + 2400; }), "it pulls the mix");
        require(reported([](const SoundDevice::Report& r) { return r.lost == 0 && r.rate == 48000 && r.bufferFrames == 1920 && r.burst == 96; }),
                "40 ms of buffer at 48 kHz in bursts of 96: " + std::to_string(report.bufferFrames));

        // 2. Unplugged while it plays (it played long enough to count as stable): replaced at once, and the new
        //    stream plays the mix on.
        std::this_thread::sleep_for(milliseconds(350));
        stream(0)->unplug = true;
        require(eventually([&] { return world.opened == 2 && playing(1); }), "replaced after the unplug");
        require(closed(0), "the unplugged stream is closed");
        before = position();
        require(eventually([&] { return position() > before + 2400; }), "the new stream pulls the mix");
        require(reported([](const SoundDevice::Report& r) { return r.lost == 1 && r.replaced == 1; }), "one lost, one replaced");

        // 3. Paused: nothing pulls the mix, and the stand-in stays out of it. Taken back in the background: the
        //    resume returns at once, though an open now takes 80 ms, and a new stream plays.
        device.setPaused(true);
        before = position();
        std::this_thread::sleep_for(milliseconds(60));
        require(position() == before && !playing(1), "paused: the mix holds still");
        stream(1)->stolen = true;
        world.openDelayMs = 80;
        auto started = steady_clock::now();
        device.setPaused(false);
        auto took = steady_clock::now() - started;
        require(took < milliseconds(40), "the resume does not wait for the open: " + std::to_string(duration_cast<milliseconds>(took).count()) + " ms");
        require(eventually([&] { return world.opened == 3 && playing(2); }), "replaced after the resume");
        require(closed(1), "the stream taken back is closed");
        require(reported([](const SoundDevice::Report& r) { return r.lost == 2 && r.replaced == 2; }), "two lost, two replaced");

        // 4. While a replacement opens (200 ms), the session figures and the game thread's service do not wait: the
        //    last figures are given meanwhile.
        world.openDelayMs = 200;
        device.setPaused(true);
        stream(2)->stolen = true;
        device.setPaused(false);
        std::this_thread::sleep_for(milliseconds(30));   // the keeper is inside the open now
        started = steady_clock::now();
        report = device.report();
        bool serviced = device.service(&health);
        took = steady_clock::now() - started;
        require(took < milliseconds(20) && !serviced, "nothing waits for the open: " + std::to_string(duration_cast<milliseconds>(took).count()) + " ms");
        require(report.lost == 2, "the figures of the last answer meanwhile");
        require(eventually([&] { return world.opened == 4 && playing(3); }), "replaced");
        world.openDelayMs = 0;
        require(reported([](const SoundDevice::Report& r) { return r.lost == 3 && r.replaced == 3; }), "three lost, three replaced");

        // 5. No stream for a while (four tries fail, each a low-latency open and a default one: tried at once, then
        //    after 50, 100, 200 and 400 ms): the stand-in mixes in real time, so the mix's play cursor moves as on a
        //    working device.
        std::this_thread::sleep_for(milliseconds(350));
        {
            std::lock_guard<std::mutex> lock(world.mutex);
            world.failOpens = 8;
        }
        stream(3)->unplug = true;
        require(eventually([&] { return closed(3); }), "the unplugged stream is closed");
        before = position();
        auto from = steady_clock::now();
        std::this_thread::sleep_for(milliseconds(300));
        double moved = position() - before, expected = 48000 * duration<double>(steady_clock::now() - from).count();
        require(world.opened == 4 && moved > 0.6 * expected && moved < 1.4 * expected,
                "the stand-in mixes in real time: " + std::to_string(moved) + " frames for " + std::to_string(expected));
        require(eventually([&] { return world.opened == 5 && playing(4); }), "a stream again once an open works");
        require(reported([](const SoundDevice::Report& r) { return r.lost == 4 && r.replaced == 4; }), "four lost, four replaced");
        require(report.silentMs >= 600 && report.silentMs < 1500, "about 750 ms without sound: " + std::to_string(report.silentMs));
        require(device.standInFrames() > 24000, "the stand-in mixed it: " + std::to_string(device.standInFrames()));

        // 6. Underruns grow the buffer by a burst; no new ones, no growth.
        stream(4)->xruns = 3;
        require(device.service(&health) && health.xruns == 3 && health.bufferFrames == 1920 + 96 && health.capacity == 4800
                    && health.lost == 4 && health.rate == 48000, "one burst more: " + std::to_string(health.bufferFrames));
        require(device.service(&health) && health.bufferFrames == 2016, "no new underruns, no growth");

        // 7. Stop: the keeper ends and every stream is closed; the record keeps the underruns.
        device.stop();
        require(world.closed == world.opened && !device.service(&health), "every stream closed");
        require(reported([](const SoundDevice::Report& r) { return r.xruns == 3 && r.lost == 4 && r.replaced == 4 && r.bufferFrames == 2016; }),
                "the record stays");
        before = position();
        std::this_thread::sleep_for(milliseconds(50));
        require(position() == before, "nothing mixes after stop");
    }
    // 8. A device that goes away while it plays (the destructor) ends as cleanly.
    {
        SoundDevice device(mixer, fast);
        device.start();
        require(eventually([&] { return playing(5); }), "plays");
    }
    require(world.closed == world.opened, "closed by the destructor");
    std::puts("sound_device: ok");
    return 0;
}
