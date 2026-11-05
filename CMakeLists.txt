cmake_minimum_required(VERSION 3.24)

project(openartemis
  VERSION 0.1.0
  DESCRIPTION "C++/SDL3 independent Artemis visual-novel engine runtime"
  LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
if(MSVC)
  add_compile_options(/utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(OA_BUILD_TOOLS "Build bundled command-line tools" OFF)
option(OA_ENABLE_TESTS "Build unit tests" OFF)
option(OA_ENABLE_ASAN "Build with AddressSanitizer (diagnostic builds only)" OFF)
option(OA_ENABLE_FUZZ "Build the parser fuzzers + the random-input monkey" OFF)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Emscripten/browser: real pthreads for the WHOLE build (core + app).
# The engine's decode/audio workers are std::thread based; a wasm module that
# mixes threaded and non-threaded objects cannot link (wasm-ld refuses
# --shared-memory when any object lacks the atomics/bulk-memory features), so
# this must be a global flag, not per-target. vcpkg's dependencies come from
# cmake/triplets/wasm32-emscripten-threads.cmake with the same flag.
# ---------------------------------------------------------------------------
if(EMSCRIPTEN)
  add_compile_options(-pthread)
  add_link_options(-pthread)
endif()

# ---------------------------------------------------------------------------
# AddressSanitizer (optional, default OFF): instrument openartemis sources so
# the full ctest suite can run under ASan (see CMakePresets.json "asan").
# Third-party deps from vcpkg stay uninstrumented, which is fine for the
# heap/stack/UAF checks this configuration is used for.
# ---------------------------------------------------------------------------
if(OA_ENABLE_ASAN)
  if(MSVC)
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/fsanitize=address /DEBUG)
  else()
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
  endif()
endif()

# ---------------------------------------------------------------------------
# 覆盖率插桩（OA_ENABLE_FUZZ）：libFuzzer 靠它做引导，必须**覆盖整个库**，
# 所以放在任何 add_subdirectory 之前全局加 —— 只给 fuzz 目标自己插桩的话
# 覆盖率只剩目标那几十个 PC，等于瞎跑。
# ---------------------------------------------------------------------------
if(OA_ENABLE_FUZZ AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-fsanitize-coverage=inline-8bit-counters,pc-table,trace-cmp)
endif()

# ---------------------------------------------------------------------------
# Dependencies (vcpkg; toolchain provided on the command line / presets)
# ---------------------------------------------------------------------------
find_package(SDL3 CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Real-asset tests: point OA_TEST_FPM_PFS at fpm/root.pfs. No machine-specific
# path is baked in: set the cache variable with -D OA_TEST_FPM_PFS=<path>, or
# export OA_TEST_FPM_PFS before configuring (honoured below). With neither,
# the variable stays empty and the tests self-skip (exit 77).
# ---------------------------------------------------------------------------
set(OA_TEST_FPM_PFS "" CACHE FILEPATH "Path to a real Artemis root.pfs for regression tests (or set the OA_TEST_FPM_PFS env var at configure time)")
if(NOT OA_TEST_FPM_PFS AND DEFINED ENV{OA_TEST_FPM_PFS})
  set(OA_TEST_FPM_PFS "$ENV{OA_TEST_FPM_PFS}" CACHE FILEPATH "" FORCE)
endif()

# U6b (P0, research/41): NekoMiko real-asset tests (nekomiko_p0_test). NekoMiko
# is the second real Artemis archive in the dev tree; it gets its own variable
# (OA_TEST_FPM_PFS stays fpm-scoped). Same rule as above: -D, or the
# OA_TEST_NEKOMIKO_PFS env var at configure time; tests self-skip (exit 77)
# when the archive is unset.
set(OA_TEST_NEKOMIKO_PFS "" CACHE FILEPATH "Path to the NekoMiko root.pfs (U6b assets; or set the OA_TEST_NEKOMIKO_PFS env var at configure time)")
if(NOT OA_TEST_NEKOMIKO_PFS AND DEFINED ENV{OA_TEST_NEKOMIKO_PFS})
  set(OA_TEST_NEKOMIKO_PFS "$ENV{OA_TEST_NEKOMIKO_PFS}" CACHE FILEPATH "" FORCE)
endif()

# research/86: rr (常轨脱离ReReCall) real-asset movie tests — the archive's
# pfs + volume chain is mounted with the archive's own real parent directory
# as the loose-file overlay (the mp4 movies ship loose in movie/), so the
# variable points at the root.pfs FILE inside the game directory. Same rule
# as the other archives: -D, or the OA_TEST_RR_PFS env var at configure
# time; tests self-skip (exit 77) when unset.
set(OA_TEST_RR_PFS "" CACHE FILEPATH "Path to the 常轨脱离ReReCall root.pfs (rr movie assets; or set the OA_TEST_RR_PFS env var at configure time)")
if(NOT OA_TEST_RR_PFS AND DEFINED ENV{OA_TEST_RR_PFS})
  set(OA_TEST_RR_PFS "$ENV{OA_TEST_RR_PFS}" CACHE FILEPATH "" FORCE)
endif()

# research/121 §5 S3 / research/122 (scheduler-approved B5): the
# new-generation E-mote archive (甜蜜女友3 = tg3) whose gesture/action
# timelines are `diff=1` with the loop=[-1,-1] sentinel — the data class
# that exposed the P0 freeze. It carries the G1 timeline regression
# (tests/fpm/emote_timeline_test.cpp). Same rule as the other archives: -D,
# or the env var at configure time; tests self-skip (exit 77) when unset.
set(OA_TEST_TG3_PFS "" CACHE FILEPATH "Path to the 甜蜜女友3 root.pfs (tg3 emote assets; or set the OA_TEST_TG3_PFS env var at configure time)")
if(NOT OA_TEST_TG3_PFS AND DEFINED ENV{OA_TEST_TG3_PFS})
  set(OA_TEST_TG3_PFS "$ENV{OA_TEST_TG3_PFS}" CACHE FILEPATH "" FORCE)
endif()

# test5 (B42): the opening recap montage + message-off input path. Guards the
# "right click, then left click leaves the story stuck" regression
# (t5_montage_input). Same rule as the other archives: -D, or the env var at
# configure time; the test self-skips (exit 77) when unset.
set(OA_TEST_T5_PFS "" CACHE FILEPATH "Path to the B42 (test5) root.pfs (or set the OA_TEST_T5_PFS env var at configure time)")
if(NOT OA_TEST_T5_PFS AND DEFINED ENV{OA_TEST_T5_PFS})
  set(OA_TEST_T5_PFS "$ENV{OA_TEST_T5_PFS}" CACHE FILEPATH "" FORCE)
endif()

set(OA_TEST_T13_PFS "" CACHE FILEPATH "Path to the test13 (Aiyoku no Eustia) root.pfs (or set the OA_TEST_T13_PFS env var at configure time)")
if(NOT OA_TEST_T13_PFS AND DEFINED ENV{OA_TEST_T13_PFS})
  set(OA_TEST_T13_PFS "$ENV{OA_TEST_T13_PFS}" CACHE FILEPATH "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
add_subdirectory(src/core)
add_subdirectory(src/app)
# tools/ (opfs / psb / asb / oasave + helper scripts) and tests/ are desktop
# host artifacts (POSIX process + real argv + window drives): the Android
# (Gradle/SDLActivity) and Emscripten (browser) shells build the plain user
# host only (research/74 §wasm/§android).
if(OA_BUILD_TOOLS AND NOT ANDROID AND NOT EMSCRIPTEN)
  add_subdirectory(tools)
endif()
if(OA_ENABLE_TESTS AND NOT ANDROID AND NOT EMSCRIPTEN)
  enable_testing()
  add_subdirectory(tests)
endif()
# 模糊测试目标跟着 tests/ 走（独立的 option：fuzz 构建通常不要跑全量单测）。
# Clang 下给整个库补覆盖率插桩，libFuzzer 才有引导信号；libFuzzer 的 main 由
# 各目标自己链（tests/fuzz/CMakeLists.txt）。
if(OA_ENABLE_FUZZ AND NOT ANDROID AND NOT EMSCRIPTEN)
  # llvm-mingw 的 clang 驱动**拒绝** -fsanitize=fuzzer（windows-gnu 目标），但
  # 运行时库还在 clang 的 resource dir 里：手动链接那个 .a，覆盖率用
  # -fsanitize-coverage 显式打开。找不到运行时库就退化成 fuzz_loop.h 的暴力
  # 变异循环（照样能配 ASan 砸内存错误）。
  set(OA_FUZZ_LIBFUZZER_OK OFF)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(OA_FUZZ_LIBFUZZER_RUNTIME "" CACHE FILEPATH
        "libFuzzer runtime archive (empty = auto-detect from clang -print-resource-dir)")
    if(NOT OA_FUZZ_LIBFUZZER_RUNTIME)
      execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
                      OUTPUT_VARIABLE _oa_clang_rt
                      OUTPUT_STRIP_TRAILING_WHITESPACE
                      RESULT_VARIABLE _oa_clang_rt_rc)
      if(_oa_clang_rt_rc EQUAL 0)
        # 先按目标架构精确匹配：同一目录里 i386/x86_64 都有，而 file(GLOB)
        # 会把所有模式的结果排序后一起给出（i386 排在前面），所以逐个模式找、
        # 命中就停。
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
          set(_oa_fuzzer_arch x86_64)
        else()
          set(_oa_fuzzer_arch i386)
        endif()
        foreach(_pat
                "${_oa_clang_rt}/lib/windows/libclang_rt.fuzzer-${_oa_fuzzer_arch}.a"
                "${_oa_clang_rt}/lib/windows/libclang_rt.fuzzer-*.a"
                "${_oa_clang_rt}/lib/*/libclang_rt.fuzzer-*.a")
          if(OA_FUZZ_LIBFUZZER_RUNTIME)
            break()
          endif()
          file(GLOB _oa_hit "${_pat}")
          if(_oa_hit)
            list(GET _oa_hit 0 OA_FUZZ_LIBFUZZER_RUNTIME)
          endif()
        endforeach()
      endif()
    endif()
    if(OA_FUZZ_LIBFUZZER_RUNTIME AND EXISTS "${OA_FUZZ_LIBFUZZER_RUNTIME}")
      set(OA_FUZZ_LIBFUZZER_OK ON)
      message(STATUS "openartemis: fuzz targets = libFuzzer (${OA_FUZZ_LIBFUZZER_RUNTIME})")
    else()
      message(STATUS "openartemis: fuzz targets = brute mode (no libFuzzer runtime)")
    endif()
  endif()
  add_subdirectory(tests/fuzz)
endif()
