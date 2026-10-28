#!/usr/bin/env python3
"""This fork's additions to the prepared DXVK tree (after nfsu2-recompiled's prepare.py + shaders.py).

Telemetry only; rendering is untouched:
  * Every DXVK log line reaches logcat (tag DXVK) at its own priority. The recipe forwards warnings and
    errors; the info lines (adapter, driver version, state-cache entries read, swapchain rebuilds) were
    written to stderr, which Android discards, and they are exactly what a performance report needs.
  * Pipeline compilations are counted and timed (graphics and compute, total and worst case) and exposed
    as an extern "C" function the game runtime reads for the session telemetry and the overlay.
  * The GPU's idle time (the HUD's "GPU load" counter: the time the submission queue had no command list
    in flight) is mirrored into a process-wide counter, so each second of a session records how busy the
    GPU was and a report can tell a GPU-bound frame from a CPU-bound one.

Usage: dxvk_extra.py <prepared dxvk-source>. Each edit must apply exactly once; otherwise the pinned
recipe changed and this script needs a look (the build fails loudly instead of silently losing data).
"""
import pathlib
import sys

STATS_HEADER = '''#pragma once
// Android telemetry (nfsu-pixel-remaster): pipeline compilation counters read by the game runtime.
#include <atomic>
#include <chrono>
#include <cstdint>

namespace dxvk::android_stats {
  extern std::atomic<uint64_t> graphicsCompiles, computeCompiles, compileNanos, compileMaxNanos, slowCompiles;
  // Microseconds the submission queue waited with nothing in flight (DxvkSubmissionQueue::finishCmdLists).
  extern std::atomic<uint64_t> gpuIdleMicros;
  inline void recordCompile(bool graphics, std::chrono::nanoseconds spent) {
    uint64_t nanos = uint64_t(spent.count() < 0 ? 0 : spent.count());
    (graphics ? graphicsCompiles : computeCompiles).fetch_add(1, std::memory_order_relaxed);
    compileNanos.fetch_add(nanos, std::memory_order_relaxed);
    if (nanos > 10000000) slowCompiles.fetch_add(1, std::memory_order_relaxed);  // over 10 ms: a visible hitch
    uint64_t seen = compileMaxNanos.load(std::memory_order_relaxed);
    while (nanos > seen && !compileMaxNanos.compare_exchange_weak(seen, nanos, std::memory_order_relaxed)) { }
  }
}
'''

STATS_SOURCE = '''#include "dxvk_android_stats.h"

namespace dxvk::android_stats {
  std::atomic<uint64_t> graphicsCompiles{0}, computeCompiles{0}, compileNanos{0}, compileMaxNanos{0}, slowCompiles{0};
  std::atomic<uint64_t> gpuIdleMicros{0};
}

// GPU idle microseconds since the process started (all devices; the game has one). Busy share over an
// interval = 1 - idle delta / interval, the same measure as DXVK's HUD "GPU load".
extern "C" uint64_t dxvkAndroidGpuIdleMicros() {
  return dxvk::android_stats::gpuIdleMicros.load(std::memory_order_relaxed);
}

// Read by the game runtime (guest_runtime.cpp): graphics compiles, compute compiles, total nanoseconds,
// worst single compile in nanoseconds, compiles slower than 10 ms.
extern "C" void dxvkAndroidPipelineStats(uint64_t out[5]) {
  using namespace dxvk::android_stats;
  out[0] = graphicsCompiles.load(std::memory_order_relaxed);
  out[1] = computeCompiles.load(std::memory_order_relaxed);
  out[2] = compileNanos.load(std::memory_order_relaxed);
  out[3] = compileMaxNanos.load(std::memory_order_relaxed);
  out[4] = slowCompiles.load(std::memory_order_relaxed);
}
'''


def replace(root: pathlib.Path, relative: str, old: str, new: str) -> None:
    path = root / relative
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"dxvk_extra: expected exactly one match in {relative}, found {count}:\n{old}")
    path.write_text(text.replace(old, new), encoding="utf-8")


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    root = pathlib.Path(sys.argv[1])
    if not (root / "ANDROID_PREPARE_SHA256").is_file():
        raise SystemExit(f"dxvk_extra: {root} is not a prepared DXVK tree (run prepare.py first)")

    # 1. All log levels to logcat, each at its own priority (the recipe only forwarded Warn and Error).
    replace(root, "src/util/log/log.cpp",
            '        if (level >= LogLevel::Warn)\n'
            '          __android_log_print(level >= LogLevel::Error ? ANDROID_LOG_ERROR : ANDROID_LOG_WARN, "DXVK", "%s", line.c_str());\n',
            '        // nfsu-pixel-remaster: every level, at its own priority; DXVK_LOG_LEVEL still decides what is emitted.\n'
            '        __android_log_print(level >= LogLevel::Error ? ANDROID_LOG_ERROR : level == LogLevel::Warn ? ANDROID_LOG_WARN\n'
            '          : level == LogLevel::Info ? ANDROID_LOG_INFO : ANDROID_LOG_DEBUG, "DXVK", "%s", line.c_str());\n')

    # 2. Pipeline compilation counters.
    (root / "src/dxvk/dxvk_android_stats.h").write_text(STATS_HEADER, encoding="utf-8")
    (root / "src/dxvk/dxvk_android_stats.cpp").write_text(STATS_SOURCE, encoding="utf-8")
    for relative, graphics in (("src/dxvk/dxvk_graphics.cpp", True), ("src/dxvk/dxvk_compute.cpp", False)):
        kind = "Graphics" if graphics else "Compute"
        create = "vkCreateGraphicsPipelines" if graphics else "vkCreateComputePipelines"
        replace(root, relative, '#include "dxvk_device.h"\n', '#include "dxvk_device.h"\n#include "dxvk_android_stats.h"\n')
        # Time every compilation (the recipe only timed them at debug log level) and record it.
        replace(root, relative,
                '    if (Logger::logLevel() <= LogLevel::Debug)\n'
                '      t0 = dxvk::high_resolution_clock::now();\n'
                '    \n'
                '    VkPipeline pipeline = VK_NULL_HANDLE;\n'
                f'    if (m_vkd->{create}(m_vkd->device(),\n',
                '    t0 = dxvk::high_resolution_clock::now();\n'
                '    \n'
                '    VkPipeline pipeline = VK_NULL_HANDLE;\n'
                f'    if (m_vkd->{create}(m_vkd->device(),\n')
        replace(root, relative,
                '    if (Logger::logLevel() <= LogLevel::Debug) {\n'
                '      t1 = dxvk::high_resolution_clock::now();\n'
                '      auto td = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);\n'
                f'      Logger::debug(str::format("Dxvk{kind}Pipeline: Finished in ", td.count(), " ms"));\n'
                '    }\n',
                '    t1 = dxvk::high_resolution_clock::now();\n'
                f'    android_stats::recordCompile({"true" if graphics else "false"}, std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0));\n'
                '    if (Logger::logLevel() <= LogLevel::Debug) {\n'
                '      auto td = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);\n'
                f'      Logger::debug(str::format("Dxvk{kind}Pipeline: Finished in ", td.count(), " ms"));\n'
                '    }\n')
    # 3. GPU idle time, mirrored from the submission queue's own counter.
    replace(root, "src/dxvk/dxvk_queue.cpp", '#include "dxvk_device.h"\n', '#include "dxvk_device.h"\n#include "dxvk_android_stats.h"\n')
    replace(root, "src/dxvk/dxvk_queue.cpp",
            '        m_gpuIdle += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();\n',
            '        auto idle = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();\n'
            '        m_gpuIdle += idle;\n'
            '        android_stats::gpuIdleMicros.fetch_add(uint64_t(idle < 0 ? 0 : idle), std::memory_order_relaxed);\n')
    (root / "NFSU_EXTRA_APPLIED").write_text("logcat-all-levels pipeline-stats gpu-idle\n", encoding="ascii")
    print(f"dxvk_extra: applied to {root}")


if __name__ == "__main__":
    main()
