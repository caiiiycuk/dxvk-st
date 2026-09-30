#include "dxvk_device.h"
#include "dxvk_queue.h"

namespace dxvk {
  
  DxvkSubmissionQueue::DxvkSubmissionQueue(DxvkDevice* device, const DxvkQueueCallback& callback)
  : m_device(device), m_callback(callback) {
    auto vk = m_device->vkd();

    VkSemaphoreTypeCreateInfo semaphoreType = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    semaphoreType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;

    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &semaphoreType };

    VkResult vrGraphics = vk->vkCreateSemaphore(vk->device(), &semaphoreInfo, nullptr, &m_semaphores.graphics);
    VkResult vrTransfer = vk->vkCreateSemaphore(vk->device(), &semaphoreInfo, nullptr, &m_semaphores.transfer);

    if (vrGraphics || vrTransfer) {
      throw DxvkError(str::format("Failed to create timeline semaphores: ",
        vrGraphics > vrTransfer ? vrGraphics : vrTransfer));
    }

#ifndef DXVK_SINGLE_THREADED
    m_submitThread = dxvk::thread([this] () { submitCmdLists(); });
    m_finishThread = dxvk::thread([this] () { finishCmdLists(); });
#endif
  }
  
  
  DxvkSubmissionQueue::~DxvkSubmissionQueue() {
    auto vk = m_device->vkd();

    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_stopped.store(true);
    }

#ifndef DXVK_SINGLE_THREADED
    m_appendCond.notify_all();
    m_submitCond.notify_all();

    m_submitThread.join();
    m_finishThread.join();
#endif

    vk->vkDestroySemaphore(vk->device(), m_semaphores.graphics, nullptr);
    vk->vkDestroySemaphore(vk->device(), m_semaphores.transfer, nullptr);
  }
  
  
  void DxvkSubmissionQueue::submit(
          DxvkSubmitInfo            submitInfo,
          DxvkLatencyInfo           latencyInfo,
          DxvkSubmitStatus*         status) {
    { std::unique_lock<dxvk::mutex> lock(m_mutex);

#ifdef DXVK_SINGLE_THREADED
      assertDrained("submit");
#else
      m_finishCond.wait(lock, [this] {
        return m_submitQueue.size() + m_finishQueue.size() <= MaxNumQueuedCommandBuffers;
      });
#endif

      DxvkSubmitEntry entry = { };
      entry.status = status;
      entry.submit = std::move(submitInfo);
      entry.latency = std::move(latencyInfo);

      m_submitQueue.push(std::move(entry));
      m_appendCond.notify_all();
    }

#ifdef DXVK_SINGLE_THREADED
    processInline();
#endif
  }


  void DxvkSubmissionQueue::present(
          DxvkPresentInfo           presentInfo,
          DxvkLatencyInfo           latencyInfo,
          DxvkSubmitStatus*         status) {
    { std::unique_lock<dxvk::mutex> lock(m_mutex);

#ifdef DXVK_SINGLE_THREADED
      assertDrained("present");
#endif

      DxvkSubmitEntry entry = { };
      entry.status  = status;
      entry.present = std::move(presentInfo);
      entry.latency = std::move(latencyInfo);

      m_submitQueue.push(std::move(entry));
      m_appendCond.notify_all();
    }

#ifdef DXVK_SINGLE_THREADED
    processInline();
#endif
  }


  void DxvkSubmissionQueue::synchronizeSubmission(
          DxvkSubmitStatus*   status) {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

#ifdef DXVK_SINGLE_THREADED
    assertDrained("synchronizeSubmission");

    if (status->result.load() == VK_NOT_READY) {
      Logger::err("DxvkSubmissionQueue::synchronizeSubmission: status was never submitted in single-threaded build");
      std::abort();
    }
#else
    m_submitCond.wait(lock, [status] {
      return status->result.load() != VK_NOT_READY;
    });
#endif
  }


  void DxvkSubmissionQueue::synchronize() {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

#ifdef DXVK_SINGLE_THREADED
    assertDrained("synchronize");
#else
    m_submitCond.wait(lock, [this] {
      return m_submitQueue.empty();
    });
#endif
  }


  void DxvkSubmissionQueue::waitForIdle() {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

#ifdef DXVK_SINGLE_THREADED
    assertDrained("waitForIdle");
#else
    m_submitCond.wait(lock, [this] {
      return m_submitQueue.empty();
    });

    m_finishCond.wait(lock, [this] {
      return m_finishQueue.empty();
    });
#endif
  }


  void DxvkSubmissionQueue::lockDeviceQueue() {
    m_mutexQueue.lock();

    if (m_callback)
      m_callback(true);
  }


  void DxvkSubmissionQueue::unlockDeviceQueue() {
    if (m_callback)
      m_callback(false);

    m_mutexQueue.unlock();
  }


#ifdef DXVK_SINGLE_THREADED
  void DxvkSubmissionQueue::assertDrained(const char* where) {
    // Every submit/present is processed inline before returning,
    // so a non-empty queue here means a wait that could never wake.
    if (!m_submitQueue.empty() || !m_finishQueue.empty()) {
      Logger::err(str::format("DxvkSubmissionQueue::", where, ": queue not drained in single-threaded build"));
      std::abort();
    }
  }


  void DxvkSubmissionQueue::processInline() {
    while (true) {
      DxvkSubmitEntry entry;

      { std::unique_lock<dxvk::mutex> lock(m_mutex);

        if (m_submitQueue.empty())
          break;

        entry = std::move(m_submitQueue.front());
      }

      submitEntry(std::move(entry));
    }

    while (true) {
      { std::unique_lock<dxvk::mutex> lock(m_mutex);

        if (m_finishQueue.empty())
          break;
      }

      finishEntry();
    }
  }
#endif


  void DxvkSubmissionQueue::submitCmdLists() {
    env::setThreadName("dxvk-submit");

    while (!m_stopped.load()) {
      DxvkSubmitEntry entry;

      { std::unique_lock<dxvk::mutex> lock(m_mutex);

        m_appendCond.wait(lock, [this] {
          return m_stopped.load() || !m_submitQueue.empty();
        });

        if (m_stopped.load())
          return;

        entry = std::move(m_submitQueue.front());
      }

      submitEntry(std::move(entry));
    }
  }


  void DxvkSubmissionQueue::submitEntry(DxvkSubmitEntry&& entry) {
    // Submit command buffer to device
    if (m_lastError != VK_ERROR_DEVICE_LOST) {
      std::lock_guard<dxvk::mutex> lock(m_mutexQueue);

      if (m_callback)
        m_callback(true);

      if (entry.submit.cmdList != nullptr) {
        if (entry.latency.tracker) {
          entry.latency.tracker->notifyQueueSubmit(entry.latency.frameId);

          if (!m_trackedSubmitId && entry.latency.frameId > m_trackedPresentId)
            m_trackedSubmitId = entry.latency.frameId;
        }

        entry.result = entry.submit.cmdList->submit(
          m_semaphores, m_timelines, m_trackedSubmitId);
        entry.timelines = m_timelines;
      } else if (entry.present.presenter != nullptr) {
        if (entry.latency.tracker)
          entry.latency.tracker->notifyQueuePresentBegin(entry.latency.frameId);

        entry.result = entry.present.presenter->presentImage(
          entry.present.frameId, entry.latency.tracker);

        if (entry.latency.tracker) {
          entry.latency.tracker->notifyQueuePresentEnd(
            entry.latency.frameId, entry.result);

          m_trackedPresentId = entry.latency.frameId;
          m_trackedSubmitId = 0u;
        }
      }

      if (m_callback)
        m_callback(false);
    } else {
      // Don't submit anything after device loss
      // so that drivers get a chance to recover
      entry.result = VK_ERROR_DEVICE_LOST;
    }

    if (entry.status)
      entry.status->result = entry.result;
    
    // On success, pass it on to the queue thread
    { std::unique_lock<dxvk::mutex> lock(m_mutex);

      bool doForward = (entry.result == VK_SUCCESS) ||
        (entry.present.presenter != nullptr && entry.result != VK_ERROR_DEVICE_LOST);

      if (doForward) {
        m_finishQueue.push(std::move(entry));
      } else {
        Logger::err(str::format("DxvkSubmissionQueue: Command submission failed: ", entry.result));
        m_lastError = entry.result;

        if (m_lastError != VK_ERROR_DEVICE_LOST)
          m_device->waitForIdle();
      }

      m_submitQueue.pop();
      m_submitCond.notify_all();
    }

    // Good time to invoke allocator tasks now since we
    // expect this to get called somewhat periodically.
    m_device->m_objects.memoryManager().performTimedTasks();
  }
  
  
  void DxvkSubmissionQueue::finishCmdLists() {
    env::setThreadName("dxvk-queue");

    while (!m_stopped.load()) {
      { std::unique_lock<dxvk::mutex> lock(m_mutex);

        if (m_finishQueue.empty()) {
          auto t0 = dxvk::high_resolution_clock::now();

          m_submitCond.wait(lock, [this] {
            return m_stopped.load() || !m_finishQueue.empty();
          });

          auto t1 = dxvk::high_resolution_clock::now();
          m_gpuIdle += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        }

        if (m_stopped.load())
          return;
      }

      finishEntry();
    }
  }


  void DxvkSubmissionQueue::finishEntry() {
    auto vk = m_device->vkd();

    std::unique_lock<dxvk::mutex> lock(m_mutex);
    DxvkSubmitEntry entry = std::move(m_finishQueue.front());
    lock.unlock();
    
    if (entry.submit.cmdList != nullptr) {
      VkResult status = m_lastError.load();

      if (status != VK_ERROR_DEVICE_LOST) {
        std::array<VkSemaphore, 2> semaphores = { m_semaphores.graphics, m_semaphores.transfer };
        std::array<uint64_t, 2> timelines = { entry.timelines.graphics, entry.timelines.transfer };

        if (entry.latency.tracker)
          entry.latency.tracker->notifyGpuExecutionBegin(entry.latency.frameId);

        VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        waitInfo.semaphoreCount = semaphores.size();
        waitInfo.pSemaphores = semaphores.data();
        waitInfo.pValues = timelines.data();

        status = vk->vkWaitSemaphores(vk->device(), &waitInfo, ~0ull);

        if (entry.latency.tracker && status == VK_SUCCESS)
          entry.latency.tracker->notifyGpuExecutionEnd(entry.latency.frameId);
      }

      if (status != VK_SUCCESS) {
        m_lastError = status;

        if (status != VK_ERROR_DEVICE_LOST)
          m_device->waitForIdle();
      }
    } else if (entry.present.presenter != nullptr) {
      // Signal the frame and then immediately destroy the reference.
      // This is necessary since the front-end may want to explicitly
      // destroy the presenter object. 
      entry.present.presenter->signalFrame(entry.present.frameId, entry.latency.tracker);
      entry.present.presenter = nullptr;
    }

    // Release resources and signal events, then immediately wake
    // up any thread that's currently waiting on a resource in
    // order to reduce delays as much as possible.
    if (entry.submit.cmdList != nullptr)
      entry.submit.cmdList->notifyObjects();

    lock.lock();
    m_finishQueue.pop();
    m_finishCond.notify_all();
    lock.unlock();

    // Free the command list and associated objects now
    if (entry.submit.cmdList != nullptr) {
      entry.submit.cmdList->reset();
      m_device->recycleCommandList(entry.submit.cmdList);
    }
  }
  
}
