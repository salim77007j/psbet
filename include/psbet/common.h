// ============================================================================
//  PSBET — common.h
//  Shared foundational types: fixed-width aliases, status codes, Result.
//
//  PSBET is an independent, clean-room PlayStation 5 runtime for PC.
//  This file contains no proprietary material.
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <utility>

namespace psb {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

constexpr u64 KiB = 1024ull;
constexpr u64 MiB = KiB * 1024ull;
constexpr u64 GiB = MiB * 1024ull;

// Engine version (semantic): major.minor.patch
constexpr u32 kVersionMajor = 0;
constexpr u32 kVersionMinor = 1;
constexpr u32 kVersionPatch = 0;

// ---------------------------------------------------------------------------
// Status codes
// ---------------------------------------------------------------------------
enum class Status {
    Ok = 0,
    InvalidArgument,
    NotFound,
    IoError,
    Unsupported,
    OutOfMemory,
    GuestCrash,
    GuestError,
    InternalError,
};

inline const char* statusName(Status s) {
    switch (s) {
        case Status::Ok:             return "Ok";
        case Status::InvalidArgument:return "InvalidArgument";
        case Status::NotFound:       return "NotFound";
        case Status::IoError:        return "IoError";
        case Status::Unsupported:    return "Unsupported";
        case Status::OutOfMemory:    return "OutOfMemory";
        case Status::GuestCrash:     return "GuestCrash";
        case Status::GuestError:     return "GuestError";
        case Status::InternalError:  return "InternalError";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Result — lightweight error-propagation primitive used across subsystems.
// ---------------------------------------------------------------------------
struct Result {
    Status status = Status::Ok;
    std::string message;

    explicit operator bool() const { return status == Status::Ok; }

    static Result ok() { return Result{}; }

    static Result fail(Status s, std::string msg) {
        Result r;
        r.status = s;
        r.message = std::move(msg);
        return r;
    }
};

// ---------------------------------------------------------------------------
// Numeric helpers
// ---------------------------------------------------------------------------
inline u64 alignUp(u64 value, u64 alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

} // namespace psb
