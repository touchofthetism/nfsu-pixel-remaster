#pragma once
// Test stub: a command list on the simulated GPU of tests/dxvk_queue/queue_test.cpp, with the members the
// submission queue calls (submit, synchronize, notifySignals, notifyResources, reset) and the test's
// bookkeeping. Each list moves through Idle -> Recording -> Submitted -> Finished -> Notified -> Resetting ->
// Idle; every step checks the one before it, so a list used out of order fails the test. The resources it
// used are held from its submission until notifyResources (between Notified and Resetting) or, without that,
// the end of its reset, as DXVK's DxvkLifetimeTracker does.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../vulkan/vulkan_presenter.h"

namespace dxvk {

  // dxvk_limits.h
  enum DxvkLimits : size_t {
    MaxNumQueuedCommandBuffers = 18,
  };

  class DxvkCommandList : public RcObject {

  public:

    enum Phase : int { Idle, Recording, Submitted, Finished, Notified, Resetting };

    DxvkCommandList();
    ~DxvkCommandList();

    VkResult submit(VkSemaphore waitSemaphore, VkSemaphore wakeSemaphore);
    VkResult synchronize();
    void notifySignals();
    void notifyResources();
    void reset();

    // Test bookkeeping.
    std::atomic<int>      phase = { Idle };
    std::atomic<uint64_t> sequence = { 0 };       // order of submission (1, 2, ...)
    std::atomic<uint32_t> submits = { 0 }, resets = { 0 };
    std::atomic<int64_t>  gpuDoneNs = { 0 };      // when the simulated GPU finishes it
    std::atomic<int64_t>  resetNs = { 0 };        // how long its next reset takes
    std::atomic<bool>     lastOfFrame = { false };
    std::atomic<uint64_t> frame = { 0 };
    std::atomic<bool>     resourcesHeld = { false };   // what D3D9's Lock waits on (the resources' use counts)
    std::atomic<int64_t>  notifiedNs = { 0 };          // when its signals went out

  };

}
