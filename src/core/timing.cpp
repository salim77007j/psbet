// ============================================================================
//  PSBET — timing.cpp (Phase 4)
// ============================================================================
#include "core/timing.h"

#include <chrono>
#include <thread>

#include "core/log.h"

namespace psb::time {

namespace {
u64 steadyUs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

TimeDomain& TimeDomain::get() {
    static TimeDomain inst;
    return inst;
}

void TimeDomain::init() {
    // Re-init (second guest run in-process) restarts the guest clock at 0 —
    // a fresh origin, never a continuation of the previous run's domain.
    virtualMode_.store(false, std::memory_order_release);
    hostStartUs_.store(steadyUs(), std::memory_order_release);
    virtualUs_.store(0, std::memory_order_release);
}

u64 TimeDomain::us() const {
    if (virtualMode_.load(std::memory_order_acquire))
        return virtualUs_.load(std::memory_order_acquire);
    const u64 start = hostStartUs_.load(std::memory_order_acquire);
    return steadyUs() - start;
}

u64 TimeDomain::ns() const {
    return us() * 1000ull;
}

void TimeDomain::setExitPoll(ExitPoll exitRequested, ExitPoll shutdownRequested) {
    exitPoll_.store(exitRequested, std::memory_order_release);
    shutdownPoll_.store(shutdownRequested, std::memory_order_release);
}

u64 TimeDomain::sleepUs(u64 us) const {
    constexpr u64 kSlice = 2000;   // 2 ms slices — teardown wakes within ~2 ms
    u64 remaining = us;
    while (remaining > 0) {
        const ExitPoll exitReq = exitPoll_.load(std::memory_order_acquire);
        const ExitPoll shutReq = shutdownPoll_.load(std::memory_order_acquire);
        if ((exitReq && exitReq()) || (shutReq && shutReq()))
            return remaining;                     // woken early by teardown
        const u64 step = remaining < kSlice ? remaining : kSlice;
        std::this_thread::sleep_for(std::chrono::microseconds(step));
        remaining -= step;
        if (virtualMode_.load(std::memory_order_acquire))
            return 0;   // virtual time does not consume wall time
    }
    return 0;
}

void TimeDomain::enableVirtual(u64 virtualStartUs) {
    virtualUs_.store(virtualStartUs, std::memory_order_release);
    virtualMode_.store(true, std::memory_order_release);
}

void TimeDomain::advanceVirtual(u64 deltaUs) {
    // Atomic fetch-add keeps concurrent virtual soaks monotonic.
    virtualUs_.fetch_add(deltaUs, std::memory_order_acq_rel);
}

void TimeDomain::disableVirtual() {
    virtualMode_.store(false, std::memory_order_release);
}

} // namespace psb::time
