// ============================================================================
//  PSBET — log.h
//  Thread-safe leveled logging to stderr. printf-style, zero dependencies.
// ============================================================================
#pragma once

namespace psb::log {

enum class Level : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };

void setLevel(Level level);
void setVerbose(bool verbose);
bool verbose();

// Core printf-style entry point (fmt checked at compile time on GCC/Clang).
void write(Level level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

} // namespace psb::log

#define LOG_TRACE(...) ::psb::log::write(::psb::log::Level::Trace, __VA_ARGS__)
#define LOG_DEBUG(...) ::psb::log::write(::psb::log::Level::Debug, __VA_ARGS__)
#define LOG_INFO(...)  ::psb::log::write(::psb::log::Level::Info,  __VA_ARGS__)
#define LOG_WARN(...)  ::psb::log::write(::psb::log::Level::Warn,  __VA_ARGS__)
#define LOG_ERROR(...) ::psb::log::write(::psb::log::Level::Error, __VA_ARGS__)
