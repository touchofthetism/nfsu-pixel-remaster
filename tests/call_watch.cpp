// Long D3D9 calls seen by the profiler's sampler (android/app/src/main/cpp/call_watch.h): short calls are
// never reported, a long one once at its end with its duration to the interval and one snapshot, two calls
// with the same token stay apart, a pause does not count as a call, and the per-method totals.
#include "../android/app/src/main/cpp/call_watch.h"
#include <cstdio>
#include <cstdlib>
#include <string>
static void require(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
int main() {
    const uint32_t base = 0x7e000000u, present = 0x7e002110u, draw = base + 2 * 4096 + 82 * 16;
    CallWatch watch(20000000ull);   // 20 ms
    int snapshots = 0;
    watch.setSnapshot([&] { ++snapshots; return std::string("queue 0 to submit, 2 on the GPU"); });
    uint64_t t = 1000000000ull;
    CallWatch::Call done;
    auto tick = [&](uint32_t func) { t += 1000000ull; return watch.sample(t, func, &done); };
    // Thousands of short calls between samples: never reported.
    for (int i = 0; i < 200; ++i) {
        watch.begin(draw); watch.end(); watch.begin(draw);
        require(!tick(0x0040ED50u), "a call seen by one sample only is not long");
        watch.end();
    }
    // One Present that lasts 31 samples.
    watch.begin(present);
    bool reported = false;
    for (int i = 0; i < 31; ++i) reported |= tick(0x0040A580u);
    require(!reported && snapshots == 1, "a long call is snapshotted once, when it crosses the threshold, and not reported while it lasts");
    watch.end();
    require(tick(0x0040A580u), "reported at the first sample after it ends");
    require(done.token == present && done.func == 0x0040A580u, "with its token and caller");
    require(done.ns == 30000000ull, "its duration to the sampling interval (first to last sample that saw it)");
    require(done.detail == "queue 0 to submit, 2 on the GPU", "and the snapshot");
    require(!tick(0), "reported once");
    // Two consecutive calls with the same token, each 15 ms: neither is long.
    watch.begin(present);
    for (int i = 0; i < 15; ++i) require(!tick(0x0040A580u), "first half");
    watch.end(); watch.begin(present);
    for (int i = 0; i < 15; ++i) require(!tick(0x0040A580u), "a new call with the same token starts its own clock");
    watch.end();
    require(!tick(0) && snapshots == 1, "neither crossed the threshold");
    // A pause in the middle of a call: not timed across it.
    watch.begin(present);
    for (int i = 0; i < 10; ++i) tick(0x0040A580u);
    watch.reset();
    t += 5000000000ull;   // five seconds later
    for (int i = 0; i < 10; ++i) require(!tick(0x0040A580u), "after the pause the call starts over");
    watch.end();
    require(!tick(0), "and is not reported");
    // A second long Present, then the totals.
    watch.begin(present);
    for (int i = 0; i < 51; ++i) tick(0x0040A580u);
    watch.end();
    require(tick(0) && done.ns == 50000000ull, "a 50 ms call");
    std::string json = watch.json(base);
    require(json == "{\"2/17\":[2,80.0,50.0]}", "per method: count, total and longest in ms");
    std::puts("call_watch: ok");
    return 0;
}
