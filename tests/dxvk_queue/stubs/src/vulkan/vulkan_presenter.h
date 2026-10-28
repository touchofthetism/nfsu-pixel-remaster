#pragma once
// Test stub: the Vulkan types, DXVK utilities and presenter that the submission queue (dxvk_queue.h and
// dxvk_queue.cpp) uses. The functions without a body here are defined by tests/dxvk_queue/queue_test.cpp.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>

#include "../util/thread.h"

typedef enum VkResult {
  VK_SUCCESS = 0,
  VK_NOT_READY = 1,
  VK_TIMEOUT = 2,
  VK_ERROR_DEVICE_LOST = -4,
} VkResult;

typedef struct VkSemaphore_T* VkSemaphore;

#ifndef VK_NULL_HANDLE
#define VK_NULL_HANDLE nullptr
#endif

namespace dxvk {

  // util/util_time.h on Linux.
  using high_resolution_clock = std::chrono::high_resolution_clock;

  namespace env {
    inline std::string getEnvVar(const char* name) {
      const char* value = std::getenv(name);
      return value ? value : "";
    }

    void setThreadName(const std::string& name);
  }

  namespace str {
    template<typename... Args>
    std::string format(const Args&... args) {
      std::stringstream stream;
      (stream << ... << args);
      return stream.str();
    }
  }

  struct Logger {
    static void err(const std::string& message);
  };

  namespace vk {
    class Presenter : public RcObject {
    public:
      VkResult presentImage();
    };
  }

}
