// oa::plat WebAssembly implementation — compiled
// when the target builds with emsdk (EMSCRIPTEN in CMake; preset wasm).
//
// Wasm answers:
//   - default_save_root(): "/save" — the IDBFS-backed virtual directory
//     mounted in init() (the only writable location that persists: every
//     write lands in MEMFS and is flushed to IndexedDB by FS.syncfs).
//     DirSaveStore's std::filesystem ops work unchanged on the virtual FS.
//   - init(): mounts IDBFS at /save (link flag -lidbfs), reads previously
//     persisted saves back from IndexedDB, and wires persistence + lifecycle
//     to the page: visibilitychange (tab hide -> persist + resume latch,
//     show -> resume latch: rAF-driven iterations stall while hidden while
//     the clock keeps running, so the host must reset its tick clock on
//     return — same gap as Android pause) and beforeunload (page close never
//     reaches SDL_AppQuit; return nullptr so no leave-dialog pops). The
//     syncfs read-back is asynchronous: an extremely early save read after
//     page open may race it.
//   - threads: the browser build now links REAL pthreads (see
//     src/app/CMakeLists.txt and cmake/triplets/wasm32-emscripten-threads.cmake:
//     the engine's decode/audio workers are std::thread based), so init() no
//     longer pins OA_EMOTE_THREADS — the emote raster uses its normal row-band
//     parallelism. Serving the page therefore requires SharedArrayBuffer, i.e.
//     COOP/COEP headers (web/serve.py sends them; web/index.html checks on
//     load and explains what is missing). Game-data preload (--preload-file …
//     .data) is per-game link time and lives in the page shell, not here.
//   - shutdown(): best-effort final persist + handler deregistration.
//     Verification level: code surface + documentation only — the wasm
//     build/runtime is left to an emsdk/browser environment (the L-配置
//     convention).
#include "platform/Platform.h"

#include <atomic>
#include <cstdlib>
#include <stdlib.h> // setenv (emscripten musl)
#include <string>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

namespace {

std::atomic<bool> s_resume{false};
bool s_inited = false;

// Flush MEMFS -> IndexedDB (fire and forget; async in JS). Safe to call
// more than once: FS queues the syncs.
void persist_now() {
    EM_ASM({
        if (typeof FS !== 'undefined' && FS.syncfs) {
            FS.syncfs(false, function(err) {
                if (err) console.warn('[plat] IDBFS syncfs(write) failed: ' + err);
            });
        }
    });
}

bool on_visibility_change(int, const EmscriptenVisibilityChangeEvent* ev, void*) {
    // Any visibility round trip matters to the host clock (see TU header);
    // latch on both edges so the reset fires on the first frame after the
    // stall regardless of delivery order vs. the rAF restart.
    s_resume.store(true);
    if (ev->hidden) persist_now(); // the tab may be killed while hidden
    return false;                  // do not consume the browser event
}

const char* on_beforeunload(int, const void*, void*) {
    persist_now(); // page close never reaches SDL_AppQuit
    return nullptr; // no leave-dialog
}

} // namespace

namespace oa::plat {

std::string default_save_root(const std::string&, bool) {
    // IDBFS mount point (created in init(); persists to IndexedDB).
    return "/save";
}

void init() {
    if (s_inited) return;
    s_inited = true;
    // Mount the persistent save dir and read previously saved data back.
    EM_ASM({
        if (typeof FS === 'undefined') {
            console.warn('[plat] no FS library linked (missing -lidbfs / FORCE_FILESYSTEM?)');
        } else {
            if (!FS.analyzePath('/save').exists) FS.mkdir('/save');
            FS.mount(IDBFS, {}, '/save');
            FS.syncfs(true, function(err) {
                if (err) console.warn('[plat] IDBFS syncfs(read) failed: ' + err);
            });
        }
    });
    // PhysicsFS (core/fs/physfs_fs.cpp: PHYSFS_init(NULL)) needs a user
    // directory: its POSIX layer reads $HOME and falls back to getpwuid(),
    // and in a browser BOTH are unavailable -> PHYSFS_init fails and the
    // project cannot be mounted at all. /save is the one directory this
    // platform guarantees (mount above), so it doubles as HOME.
    if (!std::getenv("HOME")) setenv("HOME", "/save", 1);
    emscripten_set_visibilitychange_callback(nullptr, true, on_visibility_change);
    emscripten_set_beforeunload_callback(nullptr, on_beforeunload);
    // Emote raster row bands: the browser build links real pthreads (see
    // src/app/CMakeLists.txt), so the default (hardware concurrency) is fine
    // — but keep an explicit knob documented for low-end devices.
    // An explicit override (OA_EMOTE_THREADS) is the low-end escape hatch.
}

void shutdown() {
    persist_now();
    if (s_inited) {
        emscripten_set_visibilitychange_callback(nullptr, true, nullptr);
        emscripten_set_beforeunload_callback(nullptr, nullptr);
        s_inited = false;
    }
}

bool take_lifecycle_resume() {
    return s_resume.exchange(false);
}

} // namespace oa::plat
