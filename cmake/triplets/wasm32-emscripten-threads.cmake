# wasm32-emscripten, THREAD-ENABLED (openartemis web build).
#
# Why this exists: the engine spawns real worker threads (media::DecodePool,
# MediaPlayers' per-channel audio worker, the emote raster row bands), so the
# browser build links pthreads. vcpkg's stock wasm32-emscripten triplet builds
# every dependency WITHOUT -pthread, and a non-atomics object cannot be linked
# into a shared-memory wasm module:
#
#   wasm-ld: error: --shared-memory is disallowed by physfs_fs.cpp.o
#                     because it was not compiled with 'atomics' or
#                     'bulk-memory' features.
#
# So the dependencies must be built with the same thread flags. Recipe mirrors
# krkrsdl3's overlay triplet (proven for SDL3 + emscripten): -pthread implies
# -matomics/-mbulk-memory and the shared-memory link mode.
#
# Use: CMakePresets "wasm" (VCPKG_OVERLAY_TRIPLETS + this triplet name).
# Note: the page must be served with COOP/COEP (SharedArrayBuffer) — see
# web/serve.py and the check in web/index.html.
set(VCPKG_ENV_PASSTHROUGH_UNTRACKED EMSCRIPTEN_ROOT EMSDK PATH)

if(NOT DEFINED ENV{EMSCRIPTEN_ROOT})
   find_path(EMSCRIPTEN_ROOT "emcc")
else()
   set(EMSCRIPTEN_ROOT "$ENV{EMSCRIPTEN_ROOT}")
endif()

if(NOT EMSCRIPTEN_ROOT)
   if(NOT DEFINED ENV{EMSDK})
      message(FATAL_ERROR "The emcc compiler not found in PATH")
   endif()
   set(EMSCRIPTEN_ROOT "$ENV{EMSDK}/upstream/emscripten")
endif()

if(NOT EXISTS "${EMSCRIPTEN_ROOT}/cmake/Modules/Platform/Emscripten.cmake")
   message(FATAL_ERROR "Emscripten.cmake toolchain file not found")
endif()

set(VCPKG_TARGET_ARCHITECTURE wasm32)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Emscripten)
# Chainload vcpkg's wrapper toolchain (it includes Emscripten.cmake and then
# applies VCPKG_{C,CXX,LINKER}_FLAGS, which a direct chainload would drop).
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${VCPKG_ROOT_DIR}/scripts/toolchains/emscripten.cmake")

set(VCPKG_C_FLAGS "-pthread")
set(VCPKG_CXX_FLAGS "-pthread")
set(VCPKG_LINKER_FLAGS "-pthread")

# Ports that do their own configure (autotools/make) read the environment.
set(ENV{CFLAGS} "-pthread -matomics -mbulk-memory")
set(ENV{CXXFLAGS} "-pthread -matomics -mbulk-memory")
set(ENV{LDFLAGS} "-pthread -sSHARED_MEMORY=1")
