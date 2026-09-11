# Two hosts share the core TU (main.cpp):
#   openartemis      — plain user build (no test/autodrive/probe code).
#   openartemis_test — OA_TEST_BUILD=1 superset: main_test.cpp recompiles
#                      main.cpp (small #if OA_TEST_BUILD hooks only) and
#                      app_test_drive.cpp adds the autodrive machinery in a
#                      real second TU (OA_AUTODRIVE=exit|title|help|conf|
#                      r10save|r10t|r10blog|qld|d38|nmfg|scale journeys,
#                      synthetic-input probes, pixel metrics, OA_* env hooks;
#                      see docs/TESTING.md and the file headers).
find_package(PNG CONFIG REQUIRED)
find_package(Freetype CONFIG REQUIRED)
# Host platform abstraction (oa::plat): one implementation TU
# per OS family, compiled into BOTH hosts (the layer is independent of
# OA_TEST_BUILD — the user binary and the test binary share one definition).
if(EMSCRIPTEN)
  # IDBFS save dir mount + page visibility/beforeunload hooks (build/runtime
  # verification left to an emsdk/browser environment).
  # -lidbfs provides the IDBFS filesystem; FORCE_FILESYSTEM keeps the FS
  # library linked for the EM_ASM mount code. Game-data .data preloading is
  # per-game link time in the page shell, not a preset flag.
  set(OA_HOST_PLATFORM platform/platform_wasm.cpp)
elseif(ANDROID)
  # App-private save root + SDL lifecycle latch (runtime
  # verification left to an NDK/device environment).
  set(OA_HOST_PLATFORM platform/platform_android.cpp)
else()
  # linux + windows share the desktop TU: identical save-root semantics and
  # no OS lifecycle sources on either. Split when a real
  # divergence appears.
  set(OA_HOST_PLATFORM platform/platform_desktop.cpp)
endif()
if(ANDROID)
  # Android port: SDLActivity loads the native side as
  # ONE shared library (org.libsdl.app.SDLActivity.getLibraries() ->
  # "SDL3" + the app library; see android/app/java/.../SDLActivity.java), so
  # the host is a SHARED library here, not an executable. SDL3's Android glue
  # (linked via SDL3::SDL3) provides the ANativeActivity entry that calls
  # into this library's main().
  add_library(openartemis SHARED main.cpp ${OA_HOST_PLATFORM})
  set_target_properties(openartemis PROPERTIES OUTPUT_NAME "openartemis")
else()
  add_executable(openartemis main.cpp ${OA_HOST_PLATFORM})
endif()
# src/app on the include path so host headers keep their app-relative
# spelling ("app_host.h", "platform/Platform.h") from every host TU.
target_include_directories(openartemis PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(openartemis PRIVATE libopenartemis SDL3::SDL3 PNG::PNG Freetype::Freetype)
if(NOT ANDROID AND NOT EMSCRIPTEN)
  # Desktop-only: the autodrive/probe superset host (window + real argv +
  # POSIX-ish process semantics; the Android/wasm shells ship the plain user
  # host above).
  add_executable(openartemis_test main_test.cpp app_test_drive.cpp ${OA_HOST_PLATFORM})
  target_include_directories(openartemis_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(openartemis_test PRIVATE libopenartemis SDL3::SDL3 PNG::PNG Freetype::Freetype)
endif()
if(EMSCRIPTEN)
  # Emscripten link (the shell page is web/index.html —
  # adapted from the krkrsdl3 web shell: loading screen, log panel, error
  # overlay, game-archive picker):
  #   * SUFFIX .html + OUTPUT_NAME index  -> index.html/index.js/index.wasm
  #   * INVOKE_RUN=0 + callMain: the page writes the chosen game archive into
  #     MEMFS first, then starts the engine (a browser has no argv).
  #   * ALLOW_MEMORY_GROWTH: one .pfs can be hundreds of MB inside MEMFS.
  #   * -pthread (see below): the engine's decode/audio workers are real
  #     threads; the page therefore needs SharedArrayBuffer + COOP/COEP
  #     (web/serve.py sends the headers, web/index.html checks on load).
  #   * -lidbfs.js + FORCE_FILESYSTEM: the /save mount in platform_wasm.cpp
  #     and Module.FS for the shell's save import/export.
  set_target_properties(openartemis PROPERTIES
    SUFFIX ".html"
    OUTPUT_NAME "index")
  target_link_options(openartemis PRIVATE
    -lidbfs.js
    -sFORCE_FILESYSTEM=1
    -sALLOW_MEMORY_GROWTH=1
    -sNO_EXIT_RUNTIME=1
    -sINVOKE_RUN=0
    -sSTACK_SIZE=4MB
    -sEXPORTED_RUNTIME_METHODS=FS,callMain,ccall,cwrap,UTF8ToString
    --shell-file "${CMAKE_SOURCE_DIR}/web/index.html")
  # The page shell is a link INPUT, not a source: teach the build system to
  # relink index.html when it changes (otherwise emcc silently keeps the old
  # shell baked into the previous index.html).
  set_property(TARGET openartemis APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_SOURCE_DIR}/web/index.html")
  # C++ exceptions: the host wraps project open / boot / decode in try-catch
  # (main.cpp) and libc++ throws through std::filesystem / std::runtime_error.
  # Emscripten DISABLES exception support by default -> a throw aborts the
  # module (observed as an immediate exit right after the version banner).
  # -fexceptions + DISABLE_EXCEPTION_CATCHING=0 makes catch blocks work.
  target_compile_options(openartemis PRIVATE -fexceptions)
  target_link_options(openartemis PRIVATE -fexceptions -sDISABLE_EXCEPTION_CATCHING=0)
  # Threads: the engine spawns decode/audio workers (media::DecodePool,
  # MediaPlayers' per-channel audio worker, the emote raster row bands) —
  # -pthread is set globally in the top-level CMakeLists (every object of the
  # module must carry the atomics/bulk-memory features) and the dependencies
  # come from the thread-enabled triplet. The page must therefore be served
  # with COOP/COEP (SharedArrayBuffer): web/serve.py sends the headers and
  # web/index.html checks for it at load time. PTHREAD_POOL_SIZE pre-spawns
  # the worker threads (decode pool + audio channels + emote row bands).
  target_link_options(openartemis PRIVATE -sPTHREAD_POOL_SIZE=6)
elseif(ANDROID)
  # Android: GLES3 (SDL3 renders through EGL/GLESv3 here) + SDL3's own
  # Android glue. LIBRARY_OUTPUT_DIRECTORY drops libopenartemis.so straight
  # into the Gradle source set so `gradlew assembleDebug/Release` packages it
  # with no manual copy step (build/android-arm64/jniLibs/<abi>/).
  target_link_libraries(openartemis PRIVATE EGL GLESv3)
  # Release: strip at link time (-s drops the static symtab but keeps the
  # dynamic symbols the Java side needs, e.g. SDL_main). Without it the
  # unstripped .so is ~130 MB (static ffmpeg/freetype/theora); stripped ~22 MB.
  target_link_options(openartemis PRIVATE $<$<CONFIG:Release>:-s>)
  if(OA_ANDROID_JNI_DIR)
    set_target_properties(openartemis PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${OA_ANDROID_JNI_DIR}/${ANDROID_ABI}")
  endif()
endif()
if(MSVC)
  target_compile_options(openartemis PRIVATE /W4)
else()
  target_compile_options(openartemis PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(TARGET openartemis_test)
  if(MSVC)
    target_compile_options(openartemis_test PRIVATE /W4)
  else()
    target_compile_options(openartemis_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()
