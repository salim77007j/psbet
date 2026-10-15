// ============================================================================
//  PSBET — log.cpp
// ============================================================================
#include "log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace psb::log {

namespace {

std::mutex g_mutex;
Level      g_level      = Level::Info;
bool       g_verbose    = false;
std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

const char* levelTag(Level l) {
    switch (l) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?????";
}

} // namespace

void setLevel(Level level)      { std::lock_guard<std::mutex> lk(g_mutex); g_level = level; }
void setVerbose(bool verbose)   { std::lock_guard<std::mutex> lk(g_mutex); g_verbose = verbose; if (verbose && g_level > Level::Debug) g_level = Level::Debug; }
bool verbose()                  { std::lock_guard<std::mutex> lk(g_mutex); return g_verbose; }

void write(Level level, const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (level < g_level) return;

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - g_start).count();

    std::fprintf(stderr, "[psbet +%08lld.%03lld] [%s] ", static_cast<long long>(ms / 1000),
                 static_cast<long long>(ms % 1000), levelTag(level));

    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);

    std::fputc('\n', stderr);
    std::fflush(stderr);
}

} // namespace psb::log
