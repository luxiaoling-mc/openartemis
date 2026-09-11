#pragma once
// oa::plat — host-side platform abstraction (krkr environ model, minimal
// cut).
//
// The SDL3 main-callback host (src/app/main.cpp) is shared verbatim by the
// desktop / android / wasm shells, and SDL3 already erases window, audio,
// input and events — so this layer answers only the questions that really
// differ per OS family. Core (src/core) never sees it.
//
//   default_save_root()      writable-root default when the host policy
//                            (OA_SAVE_ROOT / test autodrive temp dir) did
//                            not pick one: desktop = the game data dir,
//                            android = the app-private storage dir, wasm =
//                            the IDBFS-backed "/save" virtual dir.
//   init() / shutdown()      platform service boot/teardown. Desktop and
//                            android need nothing beyond SDL3 defaults (the
//                            no-ops are documented in each TU); wasm mounts
//                            IDBFS, reads persisted saves back and wires
//                            page-hide persistence.
//   take_lifecycle_resume()  true exactly once after every OS
//                            background->foreground round trip (android
//                            onPause/onResume, wasm tab hidden/visible;
//                            desktop never). The host resets its tick clock
//                            when this fires so the paused gap (SDL_GetTicks
//                            keeps running while the loop is blocked /
//                            rAF-stalled) never enters tick() as one giant
//                            delta.
//
// One implementation TU per OS family, picked by src/app/CMakeLists.txt:
//   platform_desktop.cpp  linux + windows share it (identical semantics
//                         today; TODO: split when a divergence appears)
//   platform_android.cpp
//   platform_wasm.cpp
#include <string>

namespace oa::plat {

/// Default writable/save root when nothing else was chosen. `data_path` is
/// the game data source the host booted (a .pfs archive path, or an
/// extracted project directory when `data_is_dir`), kept for desktop-family
/// platforms whose writable location is the data directory itself.
std::string default_save_root(const std::string& data_path, bool data_is_dir);

/// Boot-time platform services. Called once from SDL_AppInit, before the
/// save store is created (wasm needs /save mounted first).
void init();

/// Final platform teardown. Called from SDL_AppQuit. Note: on wasm the
/// page-close path never reaches SDL_AppQuit — persistence hooks are wired
/// in init() instead; shutdown() is only the graceful-exit best effort.
void shutdown();

/// True once after each OS background->foreground round trip; main-thread
/// consumer, safe to poll every frame (atomic exchange, false otherwise).
bool take_lifecycle_resume();

} // namespace oa::plat
