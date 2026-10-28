// DXVK's submission queue as ci/dxvk_extra.py patches it: dxvk_queue.h and dxvk_queue.cpp (with
// dxvk_android_stats.h and .cpp), copied unchanged from the prepared DXVK tree by ci/dxvk_queue_test.sh and
// built against the stubs next to this file. A producer records and submits command lists and presents the way
// D3D9's command stream does, on a simulated GPU that runs lists one after another. Checked:
//   1. every list is reset exactly once per submission, after the GPU finished it and after its signals were
//      notified; signals go out in submission order; a list reaches the recycler only once it is reset, and
//      is never reset twice at once (each step of a list checks the one before it);
//   2. with the recycle thread a slow reset no longer holds back the signals of the lists behind it: the
//      producer's frame-latency waits (the game's Present) shrink against DXVK_NFSU_RECYCLE=0, which
//      reproduces the build-10 stall;
//   3. with 32 lists waiting for the recycle thread, the queue thread resets the next ones itself;
//   4. destroying the queue lets the recycle thread finish every list it was handed;
//   5. with the recycle thread the resources a list used are released with its signals, before its reset and
//      whatever the recycle thread is doing (D3D9's Lock waits on them); with it off, at the end of the reset,
//      as DXVK does; exactly once either way;
//   6. the telemetry (dxvkAndroidQueueTimings) counts what happened.
// The script builds this twice, with ThreadSanitizer and with AddressSanitizer + UBSan.
#include "dxvk_device.h"
#include "dxvk_android_stats.h"
#include "dxvk_queue.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

extern "C" size_t dxvkAndroidQueueTimings(uint64_t* out, size_t count);

namespace {

  using Clock = std::chrono::steady_clock;
  constexpr int64_t Ms = 1000000;

  int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
  }

  void sleepNs(int64_t ns) {
    if (ns > 0)
      std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
  }

  [[noreturn]] void fail(const char* what) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::fflush(stderr);
    std::abort();
  }

  void require(bool ok, const char* what) {
    if (!ok)
      fail(what);
  }

  // The simulated GPU: lists run one after another, in submission order, each for gpuWorkNs.
  std::mutex gpuMutex;
  int64_t gpuFreeAtNs = 0;
  std::atomic<int64_t> gpuWorkNs = { Ms };

  // Signals: the producer's frame-latency wait watches the last notified frame.
  std::mutex signalMutex;
  std::condition_variable signalCond;
  uint64_t lastNotifiedSequence = 0, lastNotifiedFrame = 0;
  std::vector<int64_t> notifyLatencyNs;   // notified - finished on the GPU, per list
  // Resources released: after the GPU finished, and after the signals; how many before the reset.
  std::vector<int64_t> releaseLatencyNs, releaseAfterSignalsNs;
  uint64_t earlyReleases = 0, lateReleases = 0;

  void recordRelease(const dxvk::DxvkCommandList& list, bool early) {
    int64_t at = nowNs();
    std::lock_guard<std::mutex> lock(signalMutex);
    releaseLatencyNs.push_back(at - list.gpuDoneNs.load());
    releaseAfterSignalsNs.push_back(at - list.notifiedNs.load());
    (early ? earlyReleases : lateReleases)++;
  }

  std::atomic<uint64_t> loggedErrors = { 0 }, deviceWaits = { 0 }, presents = { 0 }, listsAlive = { 0 },
                        synchronizes = { 0 }, listSubmits = { 0 }, listResets = { 0 };

  std::mutex namesMutex;
  std::multiset<std::string> threadNames;

  // DxvkDevice's command list recycler (32 entries, as patched).
  std::mutex recyclerMutex;
  std::vector<dxvk::Rc<dxvk::DxvkCommandList>> recycled;

}

namespace dxvk {

  void env::setThreadName(const std::string& name) {
    std::lock_guard<std::mutex> lock(namesMutex);
    threadNames.insert(name);
  }

  void Logger::err(const std::string& message) {
    std::fprintf(stderr, "DXVK error: %s\n", message.c_str());
    loggedErrors++;
  }

  VkResult vk::Presenter::presentImage() {
    presents++;
    sleepNs(Ms / 10);
    return VK_SUCCESS;
  }

  DxvkCommandList::DxvkCommandList() { listsAlive++; }

  DxvkCommandList::~DxvkCommandList() {
    int now = phase.load();
    require(now == Idle || now == Recording, "a command list destroyed while the queue still had it");
    listsAlive--;
  }

  VkResult DxvkCommandList::submit(VkSemaphore, VkSemaphore) {
    int expected = Recording;
    require(phase.compare_exchange_strong(expected, Submitted), "submitted a list that was not being recorded");
    submits++;
    listSubmits++;
    require(!resourcesHeld.exchange(true), "a list submitted while it still held the resources of a previous submission");
    std::lock_guard<std::mutex> lock(gpuMutex);
    int64_t start = std::max(nowNs(), gpuFreeAtNs);
    gpuFreeAtNs = start + gpuWorkNs.load();
    gpuDoneNs = gpuFreeAtNs;
    return VK_SUCCESS;
  }

  VkResult DxvkCommandList::synchronize() {
    synchronizes++;
    sleepNs(gpuDoneNs.load() - nowNs());   // vkWaitForFences
    int expected = Submitted;
    require(phase.compare_exchange_strong(expected, Finished), "waited on a list that was not on the GPU");
    return VK_SUCCESS;
  }

  void DxvkCommandList::notifySignals() {
    int expected = Finished;
    require(phase.compare_exchange_strong(expected, Notified), "notified a list the GPU had not finished");
    int64_t at = nowNs();
    require(at >= gpuDoneNs.load(), "notified before the GPU finished");
    notifiedNs = at;
    std::lock_guard<std::mutex> lock(signalMutex);
    require(sequence.load() == lastNotifiedSequence + 1, "signals out of submission order");
    lastNotifiedSequence = sequence.load();
    if (lastOfFrame.load())
      lastNotifiedFrame = frame.load();
    notifyLatencyNs.push_back(at - gpuDoneNs.load());
    signalCond.notify_all();
  }

  void DxvkCommandList::notifyResources() {
    require(phase.load() == Notified, "resources released before the list's signals, or once its reset had begun");
    require(nowNs() >= gpuDoneNs.load(), "resources released before the GPU finished");
    require(resourcesHeld.exchange(false), "resources released twice");
    recordRelease(*this, true);
  }

  void DxvkCommandList::reset() {
    int expected = Notified;
    require(phase.compare_exchange_strong(expected, Resetting), "reset a list before its signals, or twice at once");
    sleepNs(resetNs.load());   // command pools first (the slow part), then the resources, as DXVK orders it
    if (resourcesHeld.exchange(false))
      recordRelease(*this, false);
    resets++;
    listResets++;
    phase.store(Idle);
  }

  void DxvkDevice::waitForIdle() {
    deviceWaits++;
  }

  void DxvkDevice::recycleCommandList(const Rc<DxvkCommandList>& cmdList) {
    require(cmdList->phase.load() == DxvkCommandList::Idle, "a list reached the recycler before its reset finished");
    std::lock_guard<std::mutex> lock(recyclerMutex);
    if (recycled.size() < 32)
      recycled.push_back(cmdList);
  }

}

namespace {

  using dxvk::DxvkCommandList;
  using dxvk::Rc;

  constexpr size_t TimingCount = 26;

  struct Scenario {
    const char* name;
    bool recycleThread;
    int frames, listsPerFrame;
    int64_t recordNs, gpuNs;
    std::function<int64_t(uint64_t sequence)> resetNs;
    int frameLatency;        // the producer waits for frame f - frameLatency (0: it never waits)
    bool keepLists;          // keep every list for the per-list checks (otherwise a full recycler frees them)
  };

  struct Result {
    int64_t frameWaitNs = 0, maxLatencyNs = 0, p99LatencyNs = 0;
    int64_t releaseP99Ns = 0, releaseMaxNs = 0, releaseAfterSignalsMaxNs = 0;
    uint64_t lists = 0, earlyReleases = 0, timings[TimingCount] = {};
  };

  Rc<DxvkCommandList> takeList(std::vector<Rc<DxvkCommandList>>* registry) {
    Rc<DxvkCommandList> list;
    { std::lock_guard<std::mutex> lock(recyclerMutex);
      if (!recycled.empty()) {
        list = recycled.back();
        recycled.pop_back();
      } }
    if (list == nullptr) {
      list = new DxvkCommandList();
      if (registry)
        registry->push_back(list);
    }
    int expected = DxvkCommandList::Idle;
    require(list->phase.compare_exchange_strong(expected, DxvkCommandList::Recording),
            "the recycler handed out a list that was still in use");
    return list;
  }

  Result run(const Scenario& s) {
    setenv("DXVK_NFSU_RECYCLE", s.recycleThread ? "1" : "0", 1);
    { std::lock_guard<std::mutex> lock(signalMutex);
      lastNotifiedSequence = 0; lastNotifiedFrame = 0; notifyLatencyNs.clear();
      releaseLatencyNs.clear(); releaseAfterSignalsNs.clear(); earlyReleases = 0; lateReleases = 0; }
    { std::lock_guard<std::mutex> lock(namesMutex); threadNames.clear(); }
    { std::lock_guard<std::mutex> lock(gpuMutex); gpuFreeAtNs = 0; }
    { std::lock_guard<std::mutex> lock(recyclerMutex); recycled.clear(); }
    gpuWorkNs = s.gpuNs;

    uint64_t before[TimingCount] = {};
    require(dxvkAndroidQueueTimings(before, TimingCount) == TimingCount, "dxvkAndroidQueueTimings: 26 values");
    uint64_t submitsBefore = listSubmits.load(), resetsBefore = listResets.load(), syncsBefore = synchronizes.load();

    Result result;
    std::vector<Rc<DxvkCommandList>> registry;
    dxvk::DxvkDevice device;
    auto queue = std::make_unique<dxvk::DxvkSubmissionQueue>(&device);
    Rc<dxvk::vk::Presenter> presenter = new dxvk::vk::Presenter();
    dxvk::DxvkSubmitStatus presentStatus;
    uint64_t sequence = 0;

    for (int f = 1; f <= s.frames; f++) {
      for (int i = 0; i < s.listsPerFrame; i++) {
        Rc<DxvkCommandList> list = takeList(s.keepLists ? &registry : nullptr);
        sleepNs(s.recordNs);
        list->sequence = ++sequence;
        list->frame = uint64_t(f);
        list->lastOfFrame = i == s.listsPerFrame - 1;
        list->resetNs = s.resetNs(sequence);
        dxvk::DxvkSubmitInfo info;
        info.cmdList  = std::move(list);
        info.waitSync = VK_NULL_HANDLE;
        info.wakeSync = VK_NULL_HANDLE;
        queue->submit(std::move(info));
      }
      // D3D9's present: wait until the previous present was submitted, then queue this one.
      queue->synchronizeSubmission(&presentStatus);
      presentStatus.result = VK_NOT_READY;
      dxvk::DxvkPresentInfo present;
      present.presenter = presenter;
      queue->present(present, &presentStatus);
      // D3D9's SyncFrameLatency: frame f - latency must be finished (its last list's signal).
      if (s.frameLatency > 0 && f > s.frameLatency) {
        int64_t start = nowNs();
        std::unique_lock<std::mutex> lock(signalMutex);
        signalCond.wait(lock, [&] { return lastNotifiedFrame >= uint64_t(f - s.frameLatency); });
        result.frameWaitNs += nowNs() - start;
      }
    }
    queue->synchronizeSubmission(&presentStatus);
    { std::unique_lock<std::mutex> lock(signalMutex);
      signalCond.wait(lock, [&] { return lastNotifiedSequence == sequence; }); }
    // Everything is notified; the recycle thread may still hold lists. The destructor lets it finish them.
    queue.reset();

    result.lists = sequence;
    require(listSubmits.load() - submitsBefore == sequence, "every list was submitted once");
    require(listResets.load() - resetsBefore == sequence, "every submitted list was reset once");
    require(synchronizes.load() - syncsBefore == sequence, "one fence wait per list");
    require(loggedErrors.load() == 0 && deviceWaits.load() == 0, "no DXVK errors, no device waits");
    for (const auto& list : registry) {
      require(list->phase.load() == DxvkCommandList::Idle, "every list idle after the queue is gone");
      require(list->resets.load() == list->submits.load(), "each list reset once per submission");
      require(!list->resourcesHeld.load(), "every list's resources released");
    }
    { std::lock_guard<std::mutex> lock(signalMutex);
      require(earlyReleases + lateReleases == sequence, "the resources of each submission released exactly once");
      require(earlyReleases == (s.recycleThread ? sequence : 0), "released with the signals exactly when the recycle thread resets");
      result.earlyReleases = earlyReleases; }
    { std::lock_guard<std::mutex> lock(namesMutex);
      require(threadNames.count("dxvk-submit") == 1 && threadNames.count("dxvk-queue") == 1, "the queue's own threads");
      require(threadNames.count("dxvk-recycle") == (s.recycleThread ? 1u : 0u), "a recycle thread exactly when enabled"); }

    uint64_t after[TimingCount] = {};
    dxvkAndroidQueueTimings(after, TimingCount);
    for (size_t i = 0; i < TimingCount; i++)
      result.timings[i] = after[i] - (i == 2 || i == 9 || i == 12 || (i >= 19 && i <= 22) || i == 25 ? 0 : before[i]);   // longest and now: not deltas
    require(result.timings[0] == sequence, "telemetry: one reset recorded per list");
    if (!s.recycleThread)
      require(result.timings[4] == sequence, "telemetry: with the thread off, the queue thread resets every list");
    require(result.timings[5] == sequence, "telemetry: one fence wait recorded per list");
    require(result.timings[7] == sequence, "telemetry: one vkQueueSubmit recorded per list");
    require(after[12] == 0, "telemetry: nothing left waiting for the recycle thread");

    std::vector<int64_t> latencies;
    { std::lock_guard<std::mutex> lock(signalMutex); latencies = notifyLatencyNs; }
    std::sort(latencies.begin(), latencies.end());
    result.maxLatencyNs = latencies.empty() ? 0 : latencies.back();
    result.p99LatencyNs = latencies.empty() ? 0 : latencies[latencies.size() * 99 / 100];
    std::vector<int64_t> releases, afterSignals;
    { std::lock_guard<std::mutex> lock(signalMutex); releases = releaseLatencyNs; afterSignals = releaseAfterSignalsNs; }
    std::sort(releases.begin(), releases.end());
    std::sort(afterSignals.begin(), afterSignals.end());
    result.releaseMaxNs = releases.empty() ? 0 : releases.back();
    result.releaseP99Ns = releases.empty() ? 0 : releases[releases.size() * 99 / 100];
    result.releaseAfterSignalsMaxNs = afterSignals.empty() ? 0 : afterSignals.back();

    std::printf("%-34s %4llu lists: frame-latency waits %6.1f ms, signal delay p99 %5.1f ms max %5.1f ms; "
                "resources released p99 %5.1f ms max %5.1f ms after the GPU (%llu with the signals); "
                "resets on the queue thread %llu\n", s.name, static_cast<unsigned long long>(sequence),
                result.frameWaitNs / 1e6, result.p99LatencyNs / 1e6, result.maxLatencyNs / 1e6,
                result.releaseP99Ns / 1e6, result.releaseMaxNs / 1e6, static_cast<unsigned long long>(result.earlyReleases),
                static_cast<unsigned long long>(result.timings[4]));
    registry.clear();
    { std::lock_guard<std::mutex> lock(recyclerMutex); recycled.clear(); }
    require(listsAlive.load() == 0, "every command list freed");
    return result;
  }

}

int main() {
  // The build-10 pattern: the command stream is the busy side, the GPU is ahead, and now and then a reset is
  // slow (one in 60 takes 15 ms). DXVK's own order holds the signals of the lists behind that reset.
  auto spiky = [](uint64_t sequence) { return sequence % 60 == 0 ? 15 * Ms : Ms / 10; };
  Scenario recycle = { "recycle thread, slow resets", true, 120, 6, Ms / 2, Ms / 5, spiky, 2, true };
  Scenario inlineReset = recycle;
  inlineReset.name = "DXVK_NFSU_RECYCLE=0, slow resets";
  inlineReset.recycleThread = false;
  Result withThread = run(recycle);
  Result withoutThread = run(inlineReset);
  require(withoutThread.maxLatencyNs >= 12 * Ms, "the simulation reproduces the stall: a slow reset delays the next signal");
  require(withoutThread.frameWaitNs >= 40 * Ms, "the simulation reproduces the stall: the producer waits on it");
  require(withThread.p99LatencyNs < 8 * Ms, "with the recycle thread, signals follow the GPU");
  require(withThread.frameWaitNs * 3 < withoutThread.frameWaitNs, "with the recycle thread, the frame-latency waits shrink");
  // D3D9's Lock waits on the resources: released right after the GPU, with the signals, whatever the resets
  // cost; DXVK's own order holds them until the slow reset ahead is done.
  require(withThread.releaseP99Ns < 8 * Ms, "with the recycle thread, resources are released as the GPU finishes");
  require(withThread.releaseAfterSignalsMaxNs < 5 * Ms, "with the recycle thread, resources are released with the signals");
  require(withoutThread.releaseMaxNs >= 12 * Ms, "with DXVK's order a slow reset holds the next resources too");

  // Resets slower than the GPU and the producer: the recycle queue fills to 32 and the queue thread resets the
  // next lists itself; lists the full recycler has no room for are freed.
  Scenario backlog = { "recycle thread, 32 behind", true, 40, 10, 0, Ms / 20, [](uint64_t) { return 3 * Ms; }, 0, false };
  Result full = run(backlog);
  require(full.timings[4] > 0, "a full recycle queue makes the queue thread reset");
  // Build 11 released them only in the reset, so a Lock waited for all 32 resets queued ahead (3 ms each here).
  require(full.releaseAfterSignalsMaxNs < 5 * Ms, "32 lists behind: resources still released with the signals");

  // Shutdown with lists still waiting for the recycle thread: the destructor lets it finish them all.
  Scenario shutdown = { "recycle thread, destroyed busy", true, 5, 6, 0, Ms / 20, [](uint64_t) { return 5 * Ms; }, 0, true };
  run(shutdown);

  std::puts("dxvk queue ok");
  return 0;
}
