// ============================================================================
//  PSBET — timing.h (Phase 4)
//  Guest time domain: the single monotonic clock every guest timing API is
//  derived from. Monotonic and drift-free by construction:
//
//      guest_time = origin + (host_now - host_start)      [wall-clock mode]
//      guest_time = vbase + virtual_offset                 [virtual mode]
//
//  There is no accumulation, no re-derivation, no re-sync against a second
//  clock — the two classic sources of emulated-clock drift cannot happen.
//  A virtual mode (test/soak only) replaces the host source with an injected
//  offset so a 24-hour guest soak can be simulated in seconds.
// ============================================================================
#pragma once

#include <atomic>
#include "psbet/common.h"

namespace psb::time {

class TimeDomain {
public:
    static TimeDomain& get();

    // Freezes the origin on the host steady clock. Must be called once per
    // guest run, before any guest code executes (Emulator does this).
    void init();

    // Guest-domain microseconds since process start. Monotonic: every call
    // returns >= the previous one on the same thread, and >= every value
    // previously observed by any thread.
    u64 us() const;

    // Guest-domain nanoseconds ("PSBET tick" = 1 ns, guest ABI v1).
    u64 ns() const;

    // Frequency of the GetProcessTimeCounter tick domain: 1_000_000_000
    // (fixed constant, documented in GUEST_ABI §7).
    static constexpr u64 kCounterFrequency = 1'000'000'000ull;

    // Cooperative sleep on the guest clock. Wakes early (returns the
    // remaining time, 0 = slept fully) when the process exit is requested or
    // engine shutdown begins — blocking guest timing primitives never stall
    // teardown (Phase-3 rule, now honored by sleeps too).
    // Declarations live in kernel_shim.h; forward-declared flags here would
    // duplicate state, so sleepUs consults them via function pointers set
    // by kernel_shim at init.
    using ExitPoll = bool (*)();
    void setExitPoll(ExitPoll exitRequested, ExitPoll shutdownRequested);
    u64  sleepUs(u64 us) const;

    // ---- Virtual time (tests + accelerated soaks ONLY) ---------------------
    // While enabled, us()/ns() are served from the virtual offset. Callers
    // must not enable this in production paths.
    void enableVirtual(u64 virtualStartUs);
    void advanceVirtual(u64 deltaUs);   // monotonic bump of virtual time
    void disableVirtual();
    bool virtualMode() const { return virtualMode_.load(std::memory_order_acquire); }

private:
    TimeDomain() = default;

    std::atomic<u64> hostStartUs_{0};    // host steady sample at init
    std::atomic<u64> virtualUs_{0};      // virtual-mode guest time
    std::atomic<bool> virtualMode_{false};

    std::atomic<ExitPoll> exitPoll_{nullptr};
    std::atomic<ExitPoll> shutdownPoll_{nullptr};
};

} // namespace psb::time
