#!/usr/bin/env bash
# Runs a command with a leak-suppression that tolerates Mesa's internal
# (driver-owned) allocations while keeping full coverage for PSBET memory.
HERE="$(cd "$(dirname "$0")" && pwd)"
export LSAN_OPTIONS="suppressions=${HERE}/lsan_vulkan.supp"
exec "$@"
