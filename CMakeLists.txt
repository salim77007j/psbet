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
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Werror)
endif()

find_package(Threads REQUIRED)

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
  src/gpu/png.cpp
  src/gpu/spirv_emit.cpp
  src/gpu/shader_cache.cpp
  src/gpu/rdna.cpp
  src/gpu/rdna_spirv.cpp
  src/gpu/rasterizer.cpp
  src/gpu/vulkan_device.cpp
  src/gpu/gpu_shim.cpp
  src/emulator.cpp
)

if(MSVC)
  target_sources(psbet_core PRIVATE src/platform/w64_thunks.asm)
endif()
target_include_directories(psbet_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(psbet_core PUBLIC Threads::Threads)

if(NOT WIN32)
  target_link_libraries(psbet_core PUBLIC ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# CLI runner
# ---------------------------------------------------------------------------
add_executable(psbet src/tools/psbet_main.cpp)
target_link_libraries(psbet PRIVATE psbet_core)

# ---------------------------------------------------------------------------
# Test runner
# ---------------------------------------------------------------------------
add_executable(psbet_tests tests/run_tests.cpp)
target_link_libraries(psbet_tests PRIVATE psbet_core)

add_executable(psbet_rdna_tests tests/rdna_tests.cpp)
target_link_libraries(psbet_rdna_tests PRIVATE psbet_core)

# Fuzzed guest corpus (Phase 3): deterministic kernel-API stress + PBFF fuzz.
add_executable(psbet_fuzz tests/corpus_fuzz.cpp)
target_link_libraries(psbet_fuzz PRIVATE psbet_core)

# ---------------------------------------------------------------------------
# Guest test homebrew (ELF images for the loader to execute).
# GNU toolchain only: these exercise the exact link recipe documented in
# docs/GUEST_ABI.md. MSVC cannot produce ELF images.
# ---------------------------------------------------------------------------
if(NOT MSVC AND NOT WIN32)
  # Guests are the "console side": they must never inherit host sanitizer
  # flags — ASan-instrumented guest code has no runtime to link against and
  # will crash the moment it executes. Force-clean compile and link.
  set(GUEST_CFLAGS -shared -fPIC -nostartfiles -O2
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
endif()

# ---------------------------------------------------------------------------
# CTest wiring
# ---------------------------------------------------------------------------
enable_testing()
add_test(NAME unit_tests COMMAND psbet_tests)
add_test(NAME rdna_tests COMMAND psbet_rdna_tests)
add_test(NAME fuzz_corpus COMMAND psbet_fuzz)
