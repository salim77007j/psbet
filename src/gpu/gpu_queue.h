// ============================================================================
//  PSBET — gpu_queue.h (Phase 5)
//  Asynchronous GPU command queue (the pipelining backbone).
//
//  Console model: a guest submit does not execute inline on the guest
//  thread. The command buffer is snapshotted into a bounded queue and a
//  dedicated GPU worker executes it while the guest builds the next frame.
//  This is exactly the hazard model of a real console GPU:
//    * the submit returns before the work completes,
//    * the device reads GPU-visible memory (inline payloads, GPU-VA
//      vertices) at EXECUTION time, not submit time,
//    * guests that mutate submitted data own the synchronization
//      (multi-buffering / fences) — the documented console contract.
//
//  Backpressure (deterministic, bounded):
//    * in-flight cap: a submit blocks (guest-cooperative, wake-aware)
//      while `maxInFlight` submissions are queued or executing — the guest
//      can be at most a couple of frames ahead;
//    * snapshot cap: a single command buffer larger than kMaxCbBytes is
//      rejected loudly instead of growing host memory silently.
//
//  Determinism: the queue is FIFO and single-worker, so command buffers
//  execute one at a time in submit order — identical results to the
//  synchronous path regardless of timing. Ordering across the two queues
//  (graphics/compute) is the guest's responsibility via sceGnmWaitQueueIdle.
// ============================================================================
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "psbet/common.h"

namespace psb::gpu {

class GpuQueue {
public:
    // Executes one snapshotted command buffer. `lastInSubmit` marks the
    // final buffer of a multi-buffer submit (frame-completion hook point).
    using Executor = bool (*)(void* user, const void* data, u32 sizeBytes,
                              bool lastInSubmit);

    // `name` is used in logs/stats ("graphics"/"compute").
    GpuQueue(const char* name, Executor exec, void* user);
    ~GpuQueue();

    GpuQueue(const GpuQueue&) = delete;
    GpuQueue& operator=(const GpuQueue&) = delete;

    // Extra wake condition for a submit blocked in backpressure (checked
    // every 2 ms slice). Device injects "kernel exit requested OR frame
    // limit hit" so a guest never waits past a process exit.
    void setWakeCondition(std::function<bool()> wake) { wake_ = std::move(wake); }

    // Enqueue one command buffer (snapshotted). Blocks while in-flight
    // submissions >= maxInFlight. Returns:
    //   0  enqueued
    //   -1 rejected (too large / shutting down)
    int submit(const void* data, u32 sizeBytes, bool lastInSubmit);

    // Wait until every enqueued buffer has executed (guest-cooperative,
    // wake-aware). Used by sceGnmWaitQueueIdle.
    void waitIdle();

    // Wake every waiter (frame-limit / exit paths).
    void requestStop();

    // Drain everything, stop the worker and join it. After this the queue
    // accepts nothing (submit returns -1). Called by Device teardown and
    // before final stats reads.
    void shutdown();

    struct Stats {
        u64  enqueued = 0;
        u64  completed = 0;
        u64  rejected = 0;
        u64  backpressureWaits = 0;
        u32  maxInFlight = 0;
        u32  inFlight = 0;          // snapshot at call time
        u64  snapshotBytes = 0;     // total CB bytes accepted
        u64  executeUs = 0;         // total worker execution time
    };
    Stats stats() const;

    static constexpr u32 kDefaultMaxInFlight = 2;
    static constexpr u32 kMaxCbBytes = 1u << 20;   // 1 MiB per command buffer

    void setMaxInFlight(u32 n) { maxInFlight_ = n < 1 ? 1 : n; }

private:
    void workerLoop();

    struct Item {
        std::vector<u8> cb;        // snapshotted command buffer
        bool lastInSubmit = false;
    };

    const char* name_;
    Executor exec_;
    void* user_;
    std::function<bool()> wake_;

    mutable std::mutex mtx_;
    std::condition_variable cvQueued_;    // worker: items available / stop
    std::condition_variable cvProgress_;  // submitters: progress / stop
    std::deque<Item> queue_;
    u32 executing_ = 0;                   // 1 while the worker runs a buffer
    bool stopped_ = false;

    u32 maxInFlight_ = kDefaultMaxInFlight;

    std::thread worker_;

    // ---- stats (atomics; readable from any thread) -------------------------
    std::atomic<u64> enqueued_{0};
    std::atomic<u64> completed_{0};
    std::atomic<u64> rejected_{0};
    std::atomic<u64> bpWaits_{0};
    std::atomic<u32> maxInFlightSeen_{0};
    std::atomic<u64> snapshotBytes_{0};
    std::atomic<u64> executeUs_{0};
};

} // namespace psb::gpu
