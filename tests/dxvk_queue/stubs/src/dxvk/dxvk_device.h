#pragma once
// Test stub: the two DxvkDevice members the submission queue calls. recycleCommandList keeps up to 32
// lists, as the patched DxvkRecycler<DxvkCommandList, 32> does (tests/dxvk_queue/queue_test.cpp).
#include "dxvk_cmdlist.h"

namespace dxvk {

  class DxvkDevice {

  public:

    void waitForIdle();

    void recycleCommandList(const Rc<DxvkCommandList>& cmdList);

  };

}
