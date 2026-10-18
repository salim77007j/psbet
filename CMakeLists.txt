cmake_minimum_required(VERSION 3.16)
project(psbet LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Strict warnings on GCC/Clang — "zero errors" is enforced at compile time.
# -Wall/-Wextra are global (third-party fetched deps compile with warnings
# visible but non-fatal); -Werror is attached per-target to PSBET's own code
# below, because vendored SDL3 sources do not commit to warning-free builds
# under our flag set.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra)
endif()

find_package(Threads REQUIRED)

# PSBET's own code builds with -Werror (see note above).
function(psbet_strict_warnings tgt)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${tgt} PRIVATE -Werror)
  endif()
endfunction()

# Windows guest-hosting shim: SysV->Win64 ABI thunks (MASM), MSVC only.
if(MSVC)
  enable_language(ASM_MASM)
endif()

# ---------------------------------------------------------------------------
# Core engine library
# ---------------------------------------------------------------------------
add_library(psbet_core STATIC
  src/core/log.cpp
  src/platform/vm.cpp
  src/core/guest_vm.cpp
  src/core/memory.cpp
  src/core/elf.cpp
  src/core/kernel_shim.cpp
  src/core/cpu_supervisor.cpp
  src/core/vfs.cpp
  src/core/pfs.cpp
  src/core/syscall_trap.cpp
  src/core/timing.cpp
  src/core/audio.cpp
  src/core/pad.cpp
  src/core/net_stubs.cpp
  src/core/pacer.cpp
  src/gpu/png.cpp
  src/gpu/spirv_emit.cpp
  src/gpu/shader_cache.cpp
  src/gpu/rdna.cpp
  src/gpu/rdna_spirv.cpp
  src/gpu/rasterizer.cpp
  src/gpu/gpu_queue.cpp
  src/gpu/vulkan_device.cpp
  src/gpu/gpu_shim.cpp
  src/emulator.cpp
)

# ---------------------------------------------------------------------------
# SDL3 (OPTIONAL, Phase 4): real audio playback, gamepad input with gyro,
# and the presentation window. Fetched and built from source so CI runners
# and user machines need no system package. OFF keeps every backend on its
# deterministic fallback (wav/null audio, scripted pad, offscreen present).
# ---------------------------------------------------------------------------
option(PSBET_WITH_SDL3 "Build with SDL3 audio/window/input backends" OFF)
if(PSBET_WITH_SDL3)
  include(FetchContent)
  set(SDL_TESTS       OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES    OFF CACHE BOOL "" FORCE)
  set(SDL_SHARED      ON  CACHE BOOL "" FORCE)
  set(SDL_STATIC      OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.30
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(SDL3)
  target_sources(psbet_core PRIVATE
    src/core/audio_sdl.cpp
    src/core/pad_sdl.cpp)
  target_link_libraries(psbet_core PUBLIC SDL3::SDL3)
  target_compile_definitions(psbet_core PUBLIC PSBET_WITH_SDL3=1)
  message(STATUS "PSBET: SDL3 enabled (audio/window/gamepad backends active)")
endif()

if(MSVC)
  target_sources(psbet_core PRIVATE src/platform/w64_thunks.asm)
endif()
target_include_directories(psbet_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(psbet_core PUBLIC Threads::Threads)
psbet_strict_warnings(psbet_core)

if(NOT WIN32)
  target_link_libraries(psbet_core PUBLIC ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# CLI runner
# ---------------------------------------------------------------------------
add_executable(psbet src/tools/psbet_main.cpp)
target_link_libraries(psbet PRIVATE psbet_core)
psbet_strict_warnings(psbet)

# ---------------------------------------------------------------------------
# Test runner
# ---------------------------------------------------------------------------
add_executable(psbet_tests tests/run_tests.cpp)
target_link_libraries(psbet_tests PRIVATE psbet_core)
psbet_strict_warnings(psbet_tests)

add_executable(psbet_rdna_tests tests/rdna_tests.cpp)
target_link_libraries(psbet_rdna_tests PRIVATE psbet_core)
psbet_strict_warnings(psbet_rdna_tests)

# Fuzzed guest corpus (Phase 3): deterministic kernel-API stress + PBFF fuzz.
add_executable(psbet_fuzz tests/corpus_fuzz.cpp)
target_link_libraries(psbet_fuzz PRIVATE psbet_core)
psbet_strict_warnings(psbet_fuzz)

# ---------------------------------------------------------------------------
# Guest test homebrew (ELF images for the loader to execute).
# GNU toolchain only: these exercise the exact link recipe documented in
# docs/GUEST_ABI.md. MSVC cannot produce ELF images.
# ---------------------------------------------------------------------------
if(NOT MSVC AND NOT WIN32)
  # Guests are the "console side": they must never inherit host sanitizer
  # flags — ASan-instrumented guest code has no runtime to link against and
  # will crash the moment it executes. Force-clean compile and link.
  # -U_FORTIFY_SOURCE: distro GCCs fortify constant-size memcpy into
  # __memcpy_chk, which Windows cannot resolve (no dlsym) — guests are
  # freestanding images, never fortified (Phase-5 Windows gate lesson).
  set(GUEST_CFLAGS -shared -fPIC -nostartfiles -O2 -Werror -U_FORTIFY_SOURCE
      -fno-sanitize=address -fno-sanitize=undefined
      -fno-stack-protector)
  set(GUEST_LDFLAGS -shared -nostartfiles -Wl,-e,psb_entry -Wl,--build-id=none
      -fno-sanitize=address -fno-sanitize=undefined
      -fno-stack-protector)

  add_library(hb_hello SHARED tests/homebrew/hello.c)
  target_include_directories(hb_hello PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(hb_hello PRIVATE ${GUEST_CFLAGS})
  target_link_options(hb_hello PRIVATE ${GUEST_LDFLAGS})
  set_target_properties(hb_hello PROPERTIES
    OUTPUT_NAME "hello" PREFIX "" SUFFIX ".elf"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

  add_library(hb_worker_exit SHARED tests/homebrew/worker_exit.c)
  target_include_directories(hb_worker_exit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(hb_worker_exit PRIVATE ${GUEST_CFLAGS})
  target_link_options(hb_worker_exit PRIVATE ${GUEST_LDFLAGS})
  set_target_properties(hb_worker_exit PROPERTIES
    OUTPUT_NAME "worker_exit" PREFIX "" SUFFIX ".elf"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

  add_library(hb_cube SHARED tests/homebrew/cube.c)
  target_include_directories(hb_cube PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(hb_cube PRIVATE ${GUEST_CFLAGS})
  target_link_options(hb_cube PRIVATE ${GUEST_LDFLAGS})
  set_target_properties(hb_cube PROPERTIES
    OUTPUT_NAME "cube" PREFIX "" SUFFIX ".elf"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

  # Phase-3 homebrew suite: ABI conformance probes, VFS/app0, EQUEUE,
  # raw-syscall containment and GPU-VA draws.
  add_library(hb_abi_probe SHARED tests/homebrew/abi_probe.c)
  target_include_directories(hb_abi_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(hb_abi_probe PRIVATE ${GUEST_CFLAGS})
  target_link_options(hb_abi_probe PRIVATE ${GUEST_LDFLAGS})
  set_target_properties(hb_abi_probe PROPERTIES
    OUTPUT_NAME "abi_probe" PREFIX "" SUFFIX ".elf"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

  add_library(hb_fs_demo SHARED tests/homebrew/fs_demo.c)
  target_include_directories(hb_fs_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(hb_fs_demo PRIVATE ${GUEST_CFLAGS})
  target_link_options(hb_fs_demo PRIVATE ${GUEST_LDFLAGS})
  set_target_properties(hb_fs_demo PROPERTIES
    OUTPUT_NAME "fs_demo" PREFIX "" SUFFIX ".elf"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

  add_library(hb_equeue_demo SHARED tests/homebrew/equeue_demo.c)
  target_include_directories(hb_equeue_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(hb_equeue_demo PRIVATE ${GUEST_CFLAGS})
  target_link_options(hb_equeue_demo PRIVATE ${GUEST_LDFLAGS})
  set_target_properties(hb_equeue_demo PROPERTIES
    OUTPUT_NAME "equeue_demo" PREFIX "" SUFFIX ".elf"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

  add_library(hb_syscall_probe SHARED tests/homebrew/syscall_probe.c)
  target_include_directories(hb_syscall_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(hb_syscall_probe PRIVATE ${GUEST_CFLAGS})
  target_link_options(hb_syscall_probe PRIVATE ${GUEST_LDFLAGS})
  set_target_properties(hb_syscall_probe PROPERTIES
    OUTPUT_NAME "syscall_probe" PREFIX "" SUFFIX ".elf"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

  add_library(hb_cube_vm SHARED tests/homebrew/cube_vm.c)
  target_include_directories(hb_cube_vm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(hb_cube_vm PRIVATE ${GUEST_CFLAGS})
  target_link_options(hb_cube_vm PRIVATE ${GUEST_LDFLAGS})
  set_target_properties(hb_cube_vm PROPERTIES
    OUTPUT_NAME "cube_vm" PREFIX "" SUFFIX ".elf"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

  # Phase-4 homebrew suite: audio out, pad input, net stub surface.
  add_library(hb_audio_tone SHARED tests/homebrew/audio_tone.c)
  target_include_directories(hb_audio_tone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(hb_audio_tone PRIVATE ${GUEST_CFLAGS})
  target_link_options(hb_audio_tone PRIVATE ${GUEST_LDFLAGS})
  set_target_properties(hb_audio_tone PROPERTIES
    OUTPUT_NAME "audio_tone" PREFIX "" SUFFIX ".elf"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

  add_library(hb_input_demo SHARED tests/homebrew/input_demo.c)
  target_include_directories(hb_input_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(hb_input_demo PRIVATE ${GUEST_CFLAGS})
  target_link_options(hb_input_demo PRIVATE ${GUEST_LDFLAGS})
  set_target_properties(hb_input_demo PROPERTIES
    OUTPUT_NAME "input_demo" PREFIX "" SUFFIX ".elf"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

  add_library(hb_net_probe SHARED tests/homebrew/net_probe.c)
  target_include_directories(hb_net_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(hb_net_probe PRIVATE ${GUEST_CFLAGS})
  target_link_options(hb_net_probe PRIVATE ${GUEST_LDFLAGS})
  set_target_properties(hb_net_probe PROPERTIES
    OUTPUT_NAME "net_probe" PREFIX "" SUFFIX ".elf"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

  # Phase-5 homebrew: async compute queue + multithreaded submitters.
  add_library(hb_compute_fill SHARED tests/homebrew/compute_fill.c)
  target_include_directories(hb_compute_fill PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(hb_compute_fill PRIVATE ${GUEST_CFLAGS})
  target_link_options(hb_compute_fill PRIVATE ${GUEST_LDFLAGS})
  set_target_properties(hb_compute_fill PROPERTIES
    OUTPUT_NAME "compute_fill" PREFIX "" SUFFIX ".elf"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

  add_library(hb_mt_submit SHARED tests/homebrew/mt_submit.c)
  target_include_directories(hb_mt_submit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(hb_mt_submit PRIVATE ${GUEST_CFLAGS})
  target_link_options(hb_mt_submit PRIVATE ${GUEST_LDFLAGS})
  set_target_properties(hb_mt_submit PROPERTIES
    OUTPUT_NAME "mt_submit" PREFIX "" SUFFIX ".elf"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

# ---------------------------------------------------------------------------
# CTest wiring
# ---------------------------------------------------------------------------
enable_testing()
add_test(NAME unit_tests COMMAND psbet_tests)
add_test(NAME rdna_tests COMMAND psbet_rdna_tests)
add_test(NAME fuzz_corpus COMMAND psbet_fuzz)
