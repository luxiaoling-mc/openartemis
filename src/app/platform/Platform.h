#pragma once
// oa::plat — the few host services that differ per OS family. This is part
// of the SDL/app layer; the engine library never sees it.
//
//   default_save_root()      writable save root (desktop = game dir,
//                            android = app storage, wasm = IDBFS /save).
//   init() / shutdown()      platform boot/teardown (wasm mounts IDBFS).
//   take_lifecycle_resume()  true once after background -> foreground, so
//                            main.cpp can reset the engine tick clock.
//
// One implementation TU per OS family, picked by src/app/CMakeLists.txt.
#include <string>

namespace oa::plat {

/// Default writable/save root when nothing else was chosen. `data_path` is
/// the game data source the host booted (a .pfs archive path, or an
/// extracted project directory when `data_is_dir`).
std::string default_save_root(const std::string& data_path, bool data_is_dir);

/// Boot-time platform services, called once before the engine starts (wasm
/// needs /save mounted before the save store is opened).
void init();

/// Final platform teardown, called after the engine host is destroyed.
void shutdown();

/// True once after each OS background->foreground round trip; main.cpp
/// resets the engine tick clock when this fires.
bool take_lifecycle_resume();

} // namespace oa::plat
