#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Where the game's one busy thread runs. The Pixel 10 Pro XL session of build 2 (telemetry) showed the
// guest render thread on a mid core at ~2.7 GHz while the big core idled at 0.5-1.6 GHz: the scheduler
// sees a single 100 % thread and has no reason to move it. Three measures, each optional and logged:
//  - affinity: the render guest thread (and nothing else) is allowed only on the fastest cluster;
//  - priority: that thread gets a lower nice value;
//  - ADPF: an APerformanceHint session (API 33+, resolved at runtime) with the render thread and DXVK's
//    worker threads, aimed at three quarters of the frame cap's period (build 15: aimed at the period itself,
//    half the frames came in late); every frame's CPU work is reported so the kernel's governor raises clocks
//    to meet it instead of settling at the cheapest "busy enough" point.
//    (DXVK's threads carry names only since build 9: until then the session held the render thread alone.)
//  - DXVK's command-stream, submit, queue and cleanup threads are kept off the render thread's cores (with
//    FastestCores and BigCore), and the command stream gets the render thread's priority.
//  - The placement is kept: Android can replace a thread's affinity (build 10 lost it nine minutes into a
//    session), so the profiler's sampler calls maintainPlacement() once a second (placement_keeper.h).
//  - With FastestCores the render thread also follows the fastest cluster as the phone caps its clocks (build
//    12): the same once-a-second check reads each cluster's allowed clock and moves the render thread (and
//    DXVK's threads off its new cores) when core_choice.h says another cluster is clearly faster. BigCore keeps
//    builds 3-11's fixed placement on the biggest core. Tested end to end against a fake cpu sysfs with real
//    threads and affinity calls in tests/host_perf.cpp.
#include "core_choice.h"

namespace host_perf {
    enum class Placement { System = 0, FastestCores = 1, BigCore = 2 };

    struct CpuTopology {
        unsigned cpus = 0;
        unsigned maxKhz = 0;
        std::string fastest;   // cpu list of the fastest cluster, e.g. "7"
        std::string summary;   // for the log: per-cpu max MHz
        std::vector<CoreChoice::Cluster> clusters;   // cpus sharing a clock, by first cpu
        std::string clusterSummary;                    // for the log: "cpu 0-1 (capacity 207), ..."
    };
    CpuTopology readTopology();

    // Called once, on the guest render thread, when the game starts presenting.
    void adoptRenderThread(Placement placement, unsigned targetFps);
    // The frame cap changed (in-game frame limit menu).
    void setTargetFps(unsigned targetFps);
    // After each Present, on the render thread: the CPU work of the frame (without the pacer's sleep). Also where
    // the render thread is followed again if the keeper lost it (build 15).
    void reportFrame(std::chrono::nanoseconds work);
    // The performance hint's target work duration for a frame cap (0: uncapped), in nanoseconds.
    int64_t targetNanos(unsigned fps);
    // Puts back any placed thread's affinity or nice value that was replaced (once a second, while playing).
    void maintainPlacement();
    // Session end.
    void release();
    // One object for the telemetry JSON; `full` (the session record, not the overlay) adds the list of moves.
    std::string statusJson(bool full);
    // The render thread and DXVK's threads as found at adoption ("render", "dxvk-cs", ...), for snapshots.
    std::vector<std::pair<std::string, int32_t>> trackedThreads();
}
