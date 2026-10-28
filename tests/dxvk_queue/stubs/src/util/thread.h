#pragma once
// Test stub for DXVK's util/thread.h: its non-Windows half (the aliases the submission queue uses) and the
// reference-counting headers it brings in. ci/dxvk_queue_test.sh copies the real util/rc headers next to
// this file from the prepared DXVK tree.
#include <condition_variable>
#include <mutex>
#include <thread>

#include "rc/util_rc.h"
#include "rc/util_rc_ptr.h"

namespace dxvk {
  using mutex              = std::mutex;
  using thread             = std::thread;
  using recursive_mutex    = std::recursive_mutex;
  using condition_variable = std::condition_variable;
}
