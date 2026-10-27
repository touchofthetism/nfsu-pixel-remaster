#pragma once
#include <chrono>
#include <cstdint>
#include <string>

// Where the game's one busy thread runs. The Pixel 10 Pro XL session of build 2 (telemetry) showed the
// guest render thread on a mid core at ~2.7 GHz while the big core idled at 0.5-1.6 GHz: the scheduler
// sees a single 100 % thread and has no reason to move it. Three measures, each optional and logged:
//  - affinity: the render guest thread (and nothing else) is allowed only on the fastest cluster;
//  - priority: that thread gets a lower nice value;
//  - ADPF: an APerformanceHint session (API 33+, resolved at runtime) with the render thread and DXVK's
//    worker threads and the frame cap as target; every frame's CPU work is reported so the kernel's
//    governor raises clocks to meet it instead of settling at the cheapest "busy enough" point.
namespace host_perf {
    enum class Placement { System = 0, FastestCores = 1 };

    struct CpuTopology {
        unsigned cpus = 0;
        unsigned maxKhz = 0;
        std::string fastest;   // cpu list of the fastest cluster, e.g. "7"
        std::string summary;   // for the log: per-cpu max MHz
    };
    CpuTopology readTopology();

    // Called once, on the guest render thread, when the game starts presenting.
    void adoptRenderThread(Placement placement, unsigned targetFps);
    // The frame cap changed (in-game frame limit menu).
    void setTargetFps(unsigned targetFps);
    // After each Present: the CPU work of the frame (without the pacer's sleep).
    void reportFrame(std::chrono::nanoseconds work);
    // Session end.
    void release();
    // One line for the telemetry JSON.
    std::string statusJson();
}
