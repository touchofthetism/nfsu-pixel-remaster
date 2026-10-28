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
  * DXVK's threads get their names (dxvk-cs, dxvk-submit, dxvk-queue, dxvk-shader, ...): the native port's
    setThreadName is an empty stub on Linux, so the game runtime's performance-hint session and CPU
    placement (host_perf.cpp), which look the threads up by name, never found them.
  * The submission queue's state, for the runtime's long-call reports (call_watch.h): how many command
    lists wait to be submitted and how many are on the GPU, whether the submit thread is inside
    vkQueuePresentKHR, the queue thread waiting on a GPU fence, the command stream blocked on a full queue;
    and every present timed (count, total, longest, over 16 ms).
  * Finished command lists are reset and recycled on their own thread (dxvk-recycle). The queue thread
    used to reset every list before it could notify the next one's signals, and the game's Present waits
    on one of those (the frame-latency signal): build 10 caught 96 Present stalls of 20-127 ms with the
    queue thread running and 11-19 finished lists queued behind it. The command list recycler holds 32
    instead of 16 so a backlog does not turn into creating and destroying lists. DXVK_NFSU_RECYCLE=0
    keeps DXVK's own order. Resets (and each of their steps), fence waits and vkQueueSubmit are timed;
    command lists and descriptor pools created and destroyed are counted.
    Nothing in this DXVK waits for a reset through the queue: D3D9's WaitForResource spins on the
    resource's use count (DxvkResource::waitIdle) and the queue's only condition-variable waiter is
    submit(), which waits for room in the submit and finish queues, not for resets.

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
  // DxvkSubmissionQueue state: entries waiting for the submit thread, command lists on the GPU, the submit
  // thread inside presentImage (vkQueuePresentKHR), the queue thread waiting on a fence, the command stream
  // waiting for room in the queue; presentImage timing.
  extern std::atomic<uint32_t> submitQueued, finishQueued, inPresent, gpuFenceWait, csQueueWait;
  extern std::atomic<uint64_t> presentCalls, presentNanos, presentMaxNanos, presentSlow;
  // Command list processing and submission: lists waiting for the recycle thread; resets (count, ns, longest,
  // over 4 ms, done on the queue thread because the recycle queue was full or disabled); fence waits;
  // vkQueueSubmit; command lists created and destroyed.
  extern std::atomic<uint32_t> recycleQueued;
  extern std::atomic<uint64_t> resetCalls, resetNanos, resetMaxNanos, resetSlow, resetInline, fenceWaits, fenceNanos,
                               submitCalls, submitNanos, submitMaxNanos, listsCreated, listsDestroyed,
                               descriptorPoolsCreated, descriptorPoolsDestroyed;
  // DxvkCommandList::reset() step by step: command pools, resources (and signals), descriptor pools,
  // buffer slices with query and event handles.
  extern std::atomic<uint64_t> resetStepNanos[4], resetStepMaxNanos[4];
  inline void recordMax(std::atomic<uint64_t>& max, uint64_t value) {
    uint64_t seen = max.load(std::memory_order_relaxed);
    while (value > seen && !max.compare_exchange_weak(seen, value, std::memory_order_relaxed)) { }
  }
  inline uint64_t toNanos(std::chrono::nanoseconds spent) { return uint64_t(spent.count() < 0 ? 0 : spent.count()); }
  inline void recordReset(std::chrono::nanoseconds spent, bool onQueueThread) {
    uint64_t nanos = toNanos(spent);
    resetCalls.fetch_add(1, std::memory_order_relaxed);
    resetNanos.fetch_add(nanos, std::memory_order_relaxed);
    if (nanos > 4000000) resetSlow.fetch_add(1, std::memory_order_relaxed);
    if (onQueueThread) resetInline.fetch_add(1, std::memory_order_relaxed);
    recordMax(resetMaxNanos, nanos);
  }
  template<typename Clock>
  inline void recordResetSteps(const typename Clock::time_point (&at)[5]) {
    for (int step = 0; step < 4; step++) {
      uint64_t nanos = toNanos(std::chrono::duration_cast<std::chrono::nanoseconds>(at[step + 1] - at[step]));
      resetStepNanos[step].fetch_add(nanos, std::memory_order_relaxed);
      recordMax(resetStepMaxNanos[step], nanos);
    }
  }
  inline void recordFenceWait(std::chrono::nanoseconds spent) {
    fenceWaits.fetch_add(1, std::memory_order_relaxed);
    fenceNanos.fetch_add(toNanos(spent), std::memory_order_relaxed);
  }
  inline void recordSubmit(std::chrono::nanoseconds spent) {
    uint64_t nanos = toNanos(spent);
    submitCalls.fetch_add(1, std::memory_order_relaxed);
    submitNanos.fetch_add(nanos, std::memory_order_relaxed);
    recordMax(submitMaxNanos, nanos);
  }
  inline void recordPresent(std::chrono::nanoseconds spent) {
    uint64_t nanos = uint64_t(spent.count() < 0 ? 0 : spent.count());
    presentCalls.fetch_add(1, std::memory_order_relaxed);
    presentNanos.fetch_add(nanos, std::memory_order_relaxed);
    if (nanos > 16000000) presentSlow.fetch_add(1, std::memory_order_relaxed);
    uint64_t seen = presentMaxNanos.load(std::memory_order_relaxed);
    while (nanos > seen && !presentMaxNanos.compare_exchange_weak(seen, nanos, std::memory_order_relaxed)) { }
  }
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

#include <cstddef>

namespace dxvk::android_stats {
  std::atomic<uint64_t> graphicsCompiles{0}, computeCompiles{0}, compileNanos{0}, compileMaxNanos{0}, slowCompiles{0};
  std::atomic<uint64_t> gpuIdleMicros{0};
  std::atomic<uint32_t> submitQueued{0}, finishQueued{0}, inPresent{0}, gpuFenceWait{0}, csQueueWait{0};
  std::atomic<uint64_t> presentCalls{0}, presentNanos{0}, presentMaxNanos{0}, presentSlow{0};
  std::atomic<uint32_t> recycleQueued{0};
  std::atomic<uint64_t> resetCalls{0}, resetNanos{0}, resetMaxNanos{0}, resetSlow{0}, resetInline{0}, fenceWaits{0}, fenceNanos{0},
                        submitCalls{0}, submitNanos{0}, submitMaxNanos{0}, listsCreated{0}, listsDestroyed{0},
                        descriptorPoolsCreated{0}, descriptorPoolsDestroyed{0};
  std::atomic<uint64_t> resetStepNanos[4]{}, resetStepMaxNanos[4]{};
}

// Command list processing totals: [0] resets, [1] their ns, [2] longest, [3] over 4 ms, [4] done on the queue
// thread (the recycle thread off or 32 lists behind), [5] fence waits, [6] their ns, [7] vkQueueSubmit calls,
// [8] their ns, [9] longest, [10] command lists created, [11] destroyed, [12] lists waiting for the recycle
// thread now, [13] descriptor pools created, [14] destroyed, [15]-[18] ns spent in each step of a reset
// (command pools, resources, descriptor pools, buffers with queries and events), [19]-[22] the longest of
// each. Writes at most `count` values and returns how many there are.
extern "C" size_t dxvkAndroidQueueTimings(uint64_t* out, size_t count) {
  using namespace dxvk::android_stats;
  const uint64_t values[] = {
    resetCalls.load(std::memory_order_relaxed), resetNanos.load(std::memory_order_relaxed),
    resetMaxNanos.load(std::memory_order_relaxed), resetSlow.load(std::memory_order_relaxed),
    resetInline.load(std::memory_order_relaxed), fenceWaits.load(std::memory_order_relaxed),
    fenceNanos.load(std::memory_order_relaxed), submitCalls.load(std::memory_order_relaxed),
    submitNanos.load(std::memory_order_relaxed), submitMaxNanos.load(std::memory_order_relaxed),
    listsCreated.load(std::memory_order_relaxed), listsDestroyed.load(std::memory_order_relaxed),
    recycleQueued.load(std::memory_order_relaxed),
    descriptorPoolsCreated.load(std::memory_order_relaxed), descriptorPoolsDestroyed.load(std::memory_order_relaxed),
    resetStepNanos[0].load(std::memory_order_relaxed), resetStepNanos[1].load(std::memory_order_relaxed),
    resetStepNanos[2].load(std::memory_order_relaxed), resetStepNanos[3].load(std::memory_order_relaxed),
    resetStepMaxNanos[0].load(std::memory_order_relaxed), resetStepMaxNanos[1].load(std::memory_order_relaxed),
    resetStepMaxNanos[2].load(std::memory_order_relaxed), resetStepMaxNanos[3].load(std::memory_order_relaxed),
  };
  const size_t total = sizeof(values) / sizeof(values[0]);
  for (size_t i = 0; i < total && i < count; i++)
    out[i] = values[i];
  return total;
}

// The submission queue now (for a snapshot during a long call) and the present totals:
// out[0] waiting to submit, [1] on the GPU, [2] in vkQueuePresentKHR, [3] waiting on a GPU fence,
// [4] command stream waiting for queue room, [5] presents, [6] their nanoseconds, [7] the longest,
// [8] presents over 16 ms.
extern "C" void dxvkAndroidQueueState(uint64_t out[9]) {
  using namespace dxvk::android_stats;
  out[0] = submitQueued.load(std::memory_order_relaxed);
  out[1] = finishQueued.load(std::memory_order_relaxed);
  out[2] = inPresent.load(std::memory_order_relaxed);
  out[3] = gpuFenceWait.load(std::memory_order_relaxed);
  out[4] = csQueueWait.load(std::memory_order_relaxed);
  out[5] = presentCalls.load(std::memory_order_relaxed);
  out[6] = presentNanos.load(std::memory_order_relaxed);
  out[7] = presentMaxNanos.load(std::memory_order_relaxed);
  out[8] = presentSlow.load(std::memory_order_relaxed);
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
    # 4. Thread names (pthread names are 15 characters; DXVK's own are shorter except dxvk-adapter-N).
    replace(root, "src/util/platform/util_env_linux.cpp", '#include <unistd.h>\n', '#include <unistd.h>\n#include <pthread.h>\n#include <cstring>\n')
    replace(root, "src/util/platform/util_env_linux.cpp",
            '  void setThreadName(const std::string& name) {\n'
            '  }\n',
            '  void setThreadName(const std::string& name) {\n'
            '    // nfsu-pixel-remaster: the stub left every DXVK thread nameless; the runtime finds them by name.\n'
            '    char posixName[16] = {};\n'
            '    std::strncpy(posixName, name.c_str(), sizeof(posixName) - 1);\n'
            '    ::pthread_setname_np(::pthread_self(), posixName);\n'
            '  }\n')
    # 5. Submission queue state and present timing.
    queue = "src/dxvk/dxvk_queue.cpp"
    relaxed = "std::memory_order_relaxed"
    replace(root, queue,
            '    m_finishCond.wait(lock, [this] {\n'
            '      return m_submitQueue.size() + m_finishQueue.size() <= MaxNumQueuedCommandBuffers;\n'
            '    });\n',
            f'    android_stats::csQueueWait.store(1, {relaxed});\n'
            '    m_finishCond.wait(lock, [this] {\n'
            '      return m_submitQueue.size() + m_finishQueue.size() <= MaxNumQueuedCommandBuffers;\n'
            '    });\n'
            f'    android_stats::csQueueWait.store(0, {relaxed});\n')
    replace(root, queue,
            '    m_pending += 1;\n'
            '    m_submitQueue.push(std::move(entry));\n',
            '    m_pending += 1;\n'
            '    m_submitQueue.push(std::move(entry));\n'
            f'    android_stats::submitQueued.store(uint32_t(m_submitQueue.size()), {relaxed});\n')
    replace(root, queue,
            '    entry.present = std::move(presentInfo);\n'
            '\n'
            '    m_submitQueue.push(std::move(entry));\n',
            '    entry.present = std::move(presentInfo);\n'
            '\n'
            '    m_submitQueue.push(std::move(entry));\n'
            f'    android_stats::submitQueued.store(uint32_t(m_submitQueue.size()), {relaxed});\n')
    replace(root, queue,
            '          status = entry.present.presenter->presentImage();\n',
            f'          android_stats::inPresent.store(1, {relaxed});\n'
            '          auto presentStart = dxvk::high_resolution_clock::now();\n'
            '          status = entry.present.presenter->presentImage();\n'
            '          android_stats::recordPresent(std::chrono::duration_cast<std::chrono::nanoseconds>(dxvk::high_resolution_clock::now() - presentStart));\n'
            f'          android_stats::inPresent.store(0, {relaxed});\n')
    replace(root, queue,
            '        if (entry.submit.cmdList != nullptr)\n'
            '          m_finishQueue.push(std::move(entry));\n',
            '        if (entry.submit.cmdList != nullptr) {\n'
            '          m_finishQueue.push(std::move(entry));\n'
            f'          android_stats::finishQueued.store(uint32_t(m_finishQueue.size()), {relaxed});\n'
            '        }\n')
    replace(root, queue,
            '      m_submitQueue.pop();\n'
            '      m_submitCond.notify_all();\n',
            '      m_submitQueue.pop();\n'
            f'      android_stats::submitQueued.store(uint32_t(m_submitQueue.size()), {relaxed});\n'
            '      m_submitCond.notify_all();\n')
    replace(root, queue,
            '        status = entry.submit.cmdList->synchronize();\n',
            f'      {{ android_stats::gpuFenceWait.store(1, {relaxed});\n'
            '        auto fenceStart = dxvk::high_resolution_clock::now();\n'
            '        status = entry.submit.cmdList->synchronize();\n'
            '        android_stats::recordFenceWait(std::chrono::duration_cast<std::chrono::nanoseconds>(dxvk::high_resolution_clock::now() - fenceStart));\n'
            f'        android_stats::gpuFenceWait.store(0, {relaxed}); }}\n')
    replace(root, queue,
            '      m_finishQueue.pop();\n'
            '      m_finishCond.notify_all();\n',
            '      m_finishQueue.pop();\n'
            f'      android_stats::finishQueued.store(uint32_t(m_finishQueue.size()), {relaxed});\n'
            '      m_finishCond.notify_all();\n')
    # 6. Command lists reset and recycled on their own thread; a larger recycler; submissions timed.
    header = "src/dxvk/dxvk_queue.h"
    replace(root, header,
            '    dxvk::thread                m_submitThread;\n',
            '    // nfsu-pixel-remaster: finished command lists are reset and recycled on their own thread\n'
            '    // (recycleCmdLists), so the queue thread notifies each list\'s signals - among them the frame-latency\n'
            '    // signal the game\'s Present waits on - as soon as the GPU is done with it, instead of after the\n'
            '    // resets of every list before it. DXVK_NFSU_RECYCLE=0 keeps DXVK\'s own order.\n'
            '    static bool nfsuRecycleThread();\n'
            '    bool                              m_recycleAsync = nfsuRecycleThread();\n'
            '    bool                              m_recycleStopped = false;\n'
            '    dxvk::mutex                       m_recycleMutex;\n'
            '    dxvk::condition_variable          m_recycleCond;\n'
            '    std::queue<Rc<DxvkCommandList>>   m_recycleQueue;\n'
            '    dxvk::thread                      m_recycleThread;\n'
            '    void recycleCmdLists();\n'
            '\n'
            '    dxvk::thread                m_submitThread;\n')
    replace(root, queue,
            '  : m_device(device),\n'
            '    m_submitThread([this] () { submitCmdLists(); }),\n',
            '  : m_device(device),\n'
            '    m_recycleThread([this] () { recycleCmdLists(); }),\n'
            '    m_submitThread([this] () { submitCmdLists(); }),\n')
    replace(root, queue,
            '    m_submitThread.join();\n'
            '    m_finishThread.join();\n'
            '  }\n',
            '    m_submitThread.join();\n'
            '    m_finishThread.join();\n'
            '\n'
            '    // Nothing reaches the recycle thread any more: let it drain its queue and stop.\n'
            '    { std::unique_lock<dxvk::mutex> lock(m_recycleMutex);\n'
            '      m_recycleStopped = true; }\n'
            '    m_recycleCond.notify_all();\n'
            '    m_recycleThread.join();\n'
            '  }\n')
    replace(root, queue,
            '      entry.submit.cmdList->notifySignals();\n'
            '      entry.submit.cmdList->reset();\n'
            '\n'
            '      m_device->recycleCommandList(entry.submit.cmdList);\n',
            '      entry.submit.cmdList->notifySignals();\n'
            '\n'
            '      // nfsu-pixel-remaster: the reset waits on the recycle thread unless it is off or 32 lists behind\n'
            '      // (then it happens here, which also holds the queue back as DXVK did).\n'
            '      bool recycled = false;\n'
            '      if (m_recycleAsync) {\n'
            '        std::lock_guard<dxvk::mutex> recycleLock(m_recycleMutex);\n'
            '        if (m_recycleQueue.size() < 32) {\n'
            '          m_recycleQueue.push(std::move(entry.submit.cmdList));\n'
            f'          android_stats::recycleQueued.store(uint32_t(m_recycleQueue.size()), {relaxed});\n'
            '          recycled = true;\n'
            '        }\n'
            '      }\n'
            '      if (recycled) {\n'
            '        m_recycleCond.notify_one();\n'
            '      } else {\n'
            '        auto resetStart = dxvk::high_resolution_clock::now();\n'
            '        entry.submit.cmdList->reset();\n'
            '        m_device->recycleCommandList(entry.submit.cmdList);\n'
            '        android_stats::recordReset(std::chrono::duration_cast<std::chrono::nanoseconds>(dxvk::high_resolution_clock::now() - resetStart), true);\n'
            '      }\n')
    replace(root, queue,
            '  void DxvkSubmissionQueue::finishCmdLists() {\n',
            '  bool DxvkSubmissionQueue::nfsuRecycleThread() {\n'
            '    return env::getEnvVar("DXVK_NFSU_RECYCLE") != "0";\n'
            '  }\n'
            '\n'
            '\n'
            '  void DxvkSubmissionQueue::recycleCmdLists() {\n'
            '    if (!m_recycleAsync)\n'
            '      return;  // DXVK_NFSU_RECYCLE=0: the queue thread resets every list itself\n'
            '\n'
            '    env::setThreadName("dxvk-recycle");\n'
            '\n'
            '    std::unique_lock<dxvk::mutex> lock(m_recycleMutex);\n'
            '\n'
            '    while (true) {\n'
            '      m_recycleCond.wait(lock, [this] {\n'
            '        return m_recycleStopped || !m_recycleQueue.empty();\n'
            '      });\n'
            '\n'
            '      if (m_recycleQueue.empty())\n'
            '        return;  // stopped, and everything handed over has been reset\n'
            '\n'
            '      Rc<DxvkCommandList> cmdList = std::move(m_recycleQueue.front());\n'
            '      m_recycleQueue.pop();\n'
            f'      android_stats::recycleQueued.store(uint32_t(m_recycleQueue.size()), {relaxed});\n'
            '      lock.unlock();\n'
            '\n'
            '      auto resetStart = dxvk::high_resolution_clock::now();\n'
            '      cmdList->reset();\n'
            '      m_device->recycleCommandList(cmdList);\n'
            '      // A list the recycler has no room for is destroyed here, off the queue thread.\n'
            '      cmdList = nullptr;\n'
            '      android_stats::recordReset(std::chrono::duration_cast<std::chrono::nanoseconds>(dxvk::high_resolution_clock::now() - resetStart), false);\n'
            '\n'
            '      lock.lock();\n'
            '    }\n'
            '  }\n'
            '\n'
            '\n'
            '  void DxvkSubmissionQueue::finishCmdLists() {\n')
    replace(root, queue,
            '          status = entry.submit.cmdList->submit(\n'
            '            entry.submit.waitSync,\n'
            '            entry.submit.wakeSync);\n',
            '          auto submitStart = dxvk::high_resolution_clock::now();\n'
            '          status = entry.submit.cmdList->submit(\n'
            '            entry.submit.waitSync,\n'
            '            entry.submit.wakeSync);\n'
            '          android_stats::recordSubmit(std::chrono::duration_cast<std::chrono::nanoseconds>(dxvk::high_resolution_clock::now() - submitStart));\n')
    # A backlog of up to 18 lists in flight used to overflow the 16-entry recycler: lists destroyed on the
    # queue thread, new ones created on the command-stream thread.
    replace(root, "src/dxvk/dxvk_device.h",
            '    DxvkRecycler<DxvkCommandList,    16> m_recycledCommandLists;\n',
            '    DxvkRecycler<DxvkCommandList,    32> m_recycledCommandLists;\n')
    replace(root, "src/dxvk/dxvk_device.cpp", '#include "dxvk_device.h"\n', '#include "dxvk_device.h"\n#include "dxvk_android_stats.h"\n')
    replace(root, "src/dxvk/dxvk_device.cpp",
            '    if (cmdList == nullptr)\n'
            '      cmdList = new DxvkCommandList(this);\n',
            '    if (cmdList == nullptr) {\n'
            '      cmdList = new DxvkCommandList(this);\n'
            f'      android_stats::listsCreated.fetch_add(1, {relaxed});\n'
            '    }\n')
    replace(root, "src/dxvk/dxvk_cmdlist.cpp", '#include "dxvk_device.h"\n', '#include "dxvk_device.h"\n#include "dxvk_android_stats.h"\n')
    replace(root, "src/dxvk/dxvk_cmdlist.cpp",
            '  DxvkCommandList::~DxvkCommandList() {\n'
            '    this->reset();\n',
            '  DxvkCommandList::~DxvkCommandList() {\n'
            f'    android_stats::listsDestroyed.fetch_add(1, {relaxed});\n'
            '    this->reset();\n')
    # Each step of a reset timed: which one is slow on this driver.
    replace(root, "src/dxvk/dxvk_cmdlist.cpp",
            '  void DxvkCommandList::reset() {\n'
            '    // Reset the command buffers while everything they reference still exists.\n'
            '    // Recycling descriptor pools and releasing resources first is legal, but\n'
            '    // the 2020 Adreno 6xx driver (Redmi Note 8, Adreno 610) then follows freed\n'
            '    // descriptor state while resetting and calls through a null pointer.\n'
            '    if ((m_graphicsPool && m_vkd->vkResetCommandPool(m_vkd->device(), m_graphicsPool, 0) != VK_SUCCESS)\n'
            '     || (m_transferPool && m_vkd->vkResetCommandPool(m_vkd->device(), m_transferPool, 0) != VK_SUCCESS))\n'
            '      Logger::err("DxvkCommandList: Failed to reset command buffer");\n'
            '\n'
            '    // Signal resources and events to\n'
            '    // avoid stalling main thread\n'
            '    m_signalTracker.reset();\n'
            '    m_resources.reset();\n'
            '\n'
            '    // Recycle heavy Vulkan objects\n'
            '    m_descriptorPoolTracker.reset();\n'
            '\n'
            '    // Return buffer memory slices\n'
            '    m_bufferTracker.reset();\n'
            '\n'
            '    // Return query and event handles\n'
            '    m_gpuQueryTracker.reset();\n'
            '    m_gpuEventTracker.reset();\n'
            '\n'
            '    // Less important stuff\n'
            '    m_statCounters.reset();\n'
            '  }\n',
            '  void DxvkCommandList::reset() {\n'
            '    // nfsu-pixel-remaster: each step timed (android_stats::recordResetSteps).\n'
            '    dxvk::high_resolution_clock::time_point at[5];\n'
            '    at[0] = dxvk::high_resolution_clock::now();\n'
            '\n'
            '    // Reset the command buffers while everything they reference still exists.\n'
            '    // Recycling descriptor pools and releasing resources first is legal, but\n'
            '    // the 2020 Adreno 6xx driver (Redmi Note 8, Adreno 610) then follows freed\n'
            '    // descriptor state while resetting and calls through a null pointer.\n'
            '    if ((m_graphicsPool && m_vkd->vkResetCommandPool(m_vkd->device(), m_graphicsPool, 0) != VK_SUCCESS)\n'
            '     || (m_transferPool && m_vkd->vkResetCommandPool(m_vkd->device(), m_transferPool, 0) != VK_SUCCESS))\n'
            '      Logger::err("DxvkCommandList: Failed to reset command buffer");\n'
            '    at[1] = dxvk::high_resolution_clock::now();\n'
            '\n'
            '    // Signal resources and events to\n'
            '    // avoid stalling main thread\n'
            '    m_signalTracker.reset();\n'
            '    m_resources.reset();\n'
            '    at[2] = dxvk::high_resolution_clock::now();\n'
            '\n'
            '    // Recycle heavy Vulkan objects\n'
            '    m_descriptorPoolTracker.reset();\n'
            '    at[3] = dxvk::high_resolution_clock::now();\n'
            '\n'
            '    // Return buffer memory slices\n'
            '    m_bufferTracker.reset();\n'
            '\n'
            '    // Return query and event handles\n'
            '    m_gpuQueryTracker.reset();\n'
            '    m_gpuEventTracker.reset();\n'
            '\n'
            '    // Less important stuff\n'
            '    m_statCounters.reset();\n'
            '    at[4] = dxvk::high_resolution_clock::now();\n'
            '    android_stats::recordResetSteps<dxvk::high_resolution_clock>(at);\n'
            '  }\n')
    replace(root, "src/dxvk/dxvk_device.cpp",
            '    if (pool == nullptr)\n'
            '      pool = new DxvkDescriptorPool(m_vkd);\n',
            '    if (pool == nullptr) {\n'
            '      pool = new DxvkDescriptorPool(m_vkd);\n'
            f'      android_stats::descriptorPoolsCreated.fetch_add(1, {relaxed});\n'
            '    }\n')
    replace(root, "src/dxvk/dxvk_descriptor.cpp", '#include "dxvk_device.h"\n', '#include "dxvk_device.h"\n#include "dxvk_android_stats.h"\n')
    replace(root, "src/dxvk/dxvk_descriptor.cpp",
            '  DxvkDescriptorPool::~DxvkDescriptorPool() {\n',
            '  DxvkDescriptorPool::~DxvkDescriptorPool() {\n'
            f'    android_stats::descriptorPoolsDestroyed.fetch_add(1, {relaxed});\n')
    (root / "NFSU_EXTRA_APPLIED").write_text("logcat-all-levels pipeline-stats gpu-idle thread-names queue-state recycle-thread\n", encoding="ascii")
    print(f"dxvk_extra: applied to {root}")


if __name__ == "__main__":
    main()
