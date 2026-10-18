// ============================================================================
//  PSBET — gpu_queue.cpp (Phase 5) — asynchronous GPU command queue.
// ============================================================================
#include "gpu_queue.h"

#include <chrono>
#include <cstring>

#include "core/log.h"

namespace psb::gpu {

GpuQueue::GpuQueue(const char* name, Executor exec, void* user)
    : name_(name), exec_(exec), user_(user) {
    worker_ = std::thread([this] { workerLoop(); });
}

GpuQueue::~GpuQueue() {
    shutdown();
}

void GpuQueue::requestStop() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stopped_ = true;
    }
    cvQueued_.notify_all();
    cvProgress_.notify_all();
}

void GpuQueue::shutdown() {
    // Drain: wait for everything already accepted to execute (its effects
    // are guest-visible state), then stop and join the worker.
    {
        std::unique_lock<std::mutex> lk(mtx_);
        cvProgress_.wait(lk, [this] {
            return stopped_ || (queue_.empty() && executing_ == 0);
        });
    }
    requestStop();
    if (worker_.joinable()) worker_.join();
}

int GpuQueue::submit(const void* data, u32 sizeBytes, bool lastInSubmit) {
    if (!data || sizeBytes == 0) return -1;
    if (sizeBytes > kMaxCbBytes) {
        LOG_ERROR("gpu-queue[%s]: CB of %u bytes exceeds the %u-byte snapshot cap",
                  name_, sizeBytes, kMaxCbBytes);
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }

    {
        std::unique_lock<std::mutex> lk(mtx_);
        // Backpressure: bounded work-ahead. Wakes on stop or via the
        // injected condition (exit / frame limit). Guest-cooperative:
        // 2 ms slices, never a hard stall.
        while (!stopped_ &&
               queue_.size() + executing_ >= maxInFlight_) {
            if (wake_ && wake_()) break;
            bpWaits_.fetch_add(1, std::memory_order_relaxed);
            cvProgress_.wait_for(lk, std::chrono::milliseconds(2));
        }
        if (stopped_) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return -1;
        }

        Item it;
        it.cb.resize(sizeBytes);
        std::memcpy(it.cb.data(), data, sizeBytes);
        it.lastInSubmit = lastInSubmit;
        queue_.push_back(std::move(it));

        enqueued_.fetch_add(1, std::memory_order_acq_rel);
        snapshotBytes_.fetch_add(sizeBytes, std::memory_order_relaxed);
        const u32 infl = static_cast<u32>(queue_.size() + executing_);
        u32 seen = maxInFlightSeen_.load(std::memory_order_relaxed);
        while (infl > seen &&
               !maxInFlightSeen_.compare_exchange_weak(seen, infl,
                                                       std::memory_order_relaxed)) {
        }
    }
    cvQueued_.notify_one();
    return 0;
}

void GpuQueue::waitIdle() {
    std::unique_lock<std::mutex> lk(mtx_);
    cvProgress_.wait(lk, [this] {
        return stopped_ || (queue_.empty() && executing_ == 0);
    });
}

GpuQueue::Stats GpuQueue::stats() const {
    Stats s;
    s.enqueued = enqueued_.load(std::memory_order_relaxed);
    s.completed = completed_.load(std::memory_order_relaxed);
    s.rejected = rejected_.load(std::memory_order_relaxed);
    s.backpressureWaits = bpWaits_.load(std::memory_order_relaxed);
    s.maxInFlight = maxInFlightSeen_.load(std::memory_order_relaxed);
    s.snapshotBytes = snapshotBytes_.load(std::memory_order_relaxed);
    s.executeUs = executeUs_.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(mtx_);
    s.inFlight = static_cast<u32>(queue_.size() + executing_);
    return s;
}

void GpuQueue::workerLoop() {
    for (;;) {
        Item it;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cvQueued_.wait(lk, [this] {
                return stopped_ || !queue_.empty();
            });
            if (stopped_ && queue_.empty()) return;
            it = std::move(queue_.front());
            queue_.pop_front();
            executing_ = 1;
        }
        cvProgress_.notify_all();   // a slot freed for submitters

        const u64 t0 = static_cast<u64>(std::chrono::duration_cast<
            std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        // A malformed buffer logs and is skipped (the queue stays alive —
        // same policy as the synchronous executor: report, don't die).
        (void)exec_(user_, it.cb.data(), static_cast<u32>(it.cb.size()),
                    it.lastInSubmit);
        const u64 t1 = static_cast<u64>(std::chrono::duration_cast<
            std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        executeUs_.fetch_add(t1 - t0, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lk(mtx_);
            executing_ = 0;
            completed_.fetch_add(1, std::memory_order_acq_rel);
        }
        cvProgress_.notify_all();
        if (stopped_ && queue_.empty()) return;
    }
}

} // namespace psb::gpu
