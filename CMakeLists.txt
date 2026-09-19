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
option(OA_BUILD_TOOLS "Build bundled command-line tools" ON)
option(OA_ENABLE_TESTS "Build unit tests" ON)
option(OA_ENABLE_ASAN "Build with AddressSanitizer (diagnostic builds only)" OFF)

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

# research/87: HCT (灵感满溢的甜蜜创想凸 / Hamidashi Creative Totsu)
# real-asset movie tests — the pfs + volume chain is mounted with the
# archive's own real parent directory as the loose-file overlay (the mp4
# movies ship loose in movie_bai/), so the variable points at the root.pfs
# FILE inside the game directory. Same rule as the other archives: -D, or
# the OA_TEST_HCT_PFS env var at configure time; tests self-skip (exit 77)
# when unset.
set(OA_TEST_HCT_PFS "" CACHE FILEPATH "Path to the 灵感满溢的甜蜜创想凸 root.pfs (movie_bai assets; or set the OA_TEST_HCT_PFS env var at configure time)")
if(NOT OA_TEST_HCT_PFS AND DEFINED ENV{OA_TEST_HCT_PFS})
  set(OA_TEST_HCT_PFS "$ENV{OA_TEST_HCT_PFS}" CACHE FILEPATH "" FORCE)
endif()

# research/121 §5 S3 / research/122 (scheduler-approved B5): the two
# new-generation E-mote archives (甜蜜女友3 = tg3, きら☆かの = slny) whose
# gesture/action timelines are `diff=1` with the loop=[-1,-1] sentinel — the
# data class that exposed the P0 freeze. They carry the G1 timeline
# regression (tests/fpm/emote_timeline_test.cpp). Same rule as the other
# archives: -D, or the env var at configure time; tests self-skip (exit 77)
# when unset.
set(OA_TEST_TG3_PFS "" CACHE FILEPATH "Path to the 甜蜜女友3 root.pfs (tg3 emote assets; or set the OA_TEST_TG3_PFS env var at configure time)")
if(NOT OA_TEST_TG3_PFS AND DEFINED ENV{OA_TEST_TG3_PFS})
  set(OA_TEST_TG3_PFS "$ENV{OA_TEST_TG3_PFS}" CACHE FILEPATH "" FORCE)
endif()
set(OA_TEST_SLNY_PFS "" CACHE FILEPATH "Path to the きら☆かの root.pfs (slny emote assets; or set the OA_TEST_SLNY_PFS env var at configure time)")
if(NOT OA_TEST_SLNY_PFS AND DEFINED ENV{OA_TEST_SLNY_PFS})
  set(OA_TEST_SLNY_PFS "$ENV{OA_TEST_SLNY_PFS}" CACHE FILEPATH "" FORCE)
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
