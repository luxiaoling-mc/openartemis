// oa::plat Android implementation — compiled when the
// target builds with the NDK (ANDROID in CMake; preset android-arm64 /
// android-x64).
//
// Android answers:
//   - default_save_root(): the app-private internal storage dir
//     (SDL_GetAndroidInternalStoragePath, SDL_system.h) — the only place an
//     Android app may write without permissions. Saves land next to the
//     shell's data copy at the internal-storage root, mirroring the desktop
//     "saves beside the data file" layout. SDL3 renamed the SDL2-era
//     SDL_AndroidGetInternalStoragePath to SDL_GetAndroidInternalStoragePath
//     (verified against the vcpkg 3.4.12 headers).
//   - lifecycle: Android pauses/resumes the app around onPause/onResume.
//     SDL3 delivers SDL_EVENT_DID_ENTER_BACKGROUND / _FOREGROUND only via
//     SDL_AddEventWatch (SDL_events.h: these application events must be
//     handled in an event watch; they do NOT arrive in SDL_AppEvent). With
//     the default SDL_HINT_ANDROID_BLOCK_ON_PAUSE=1 SDL blocks the main
//     thread while paused, so the loop stops by itself and the ONLY host
//     concern is the resumed clock gap — latched here and consumed by the
//     host through take_lifecycle_resume(). Both edges latch: the host resets
//     its clock on the first frame after whichever edge is observed first
//     (robust against UI-thread/main-thread delivery ordering).
//     Verification level: code surface only — runtime behaviour on a device
//     (event thread, ordering vs. the block) is left to an NDK/device
//     environment.
//   - init()/shutdown(): no platform services beyond SDL3 defaults; the
//     event watch is registered on init and removed on shutdown.
#include "platform/Platform.h"

#include <atomic>
#include <cstdlib> // getenv/setenv (HOME for PhysicsFS, see init())
#include <filesystem>
#include <string>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_system.h>

namespace {

std::atomic<bool> s_resume{false};

// Event watch receiving the iOS/Android-only application events (see the TU
// header comment). Runs on the OS thread that SDL delivers them from —
// only atomic state changes here, no SDL calls (re-entrancy) and no host
// state (thread safety).
bool lifecycle_watch(void*, SDL_Event* ev) {
    if (ev->type == SDL_EVENT_DID_ENTER_BACKGROUND ||
        ev->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
        s_resume.store(true);
    }
    return true; // pass the event through untouched
}

} // namespace

namespace oa::plat {

std::string default_save_root(const std::string& data_path, bool data_is_dir) {
    if (const char* p = SDL_GetAndroidInternalStoragePath(); p && *p) {
        // App-private writable dir (/data/data/<pkg>/files, no permissions
        // needed). Documented to be available after SDL_Init on Android.
        return p;
    }
    // SDL storage not available yet (should not happen in a booted shell):
    // fall back to the desktop rule so saves stay next to the data file.
    std::string save_root = (data_is_dir ? std::filesystem::path(data_path)
                                         : std::filesystem::path(data_path).parent_path())
                                .string();
    if (save_root.empty()) save_root = ".";
    return save_root;
}

void init() {
    // SDL3 events need no explicit subsystem init; registering the watch at
    // AppInit time covers every later onPause/onResume (the first one cannot
    // fire before the window/activity is up, which happens after AppInit).
    SDL_AddEventWatch(lifecycle_watch, nullptr);
    // PhysicsFS (core/fs/physfs_fs.cpp: PHYSFS_init(NULL)) needs a user
    // directory: its POSIX layer reads $HOME and falls back to getpwuid(),
    // and an Android app has neither -> PHYSFS_init fails and the game data
    // is never mounted. Point HOME at the app-private storage dir (always
    // present and writable, see default_save_root above).
    if (!std::getenv("HOME")) {
        if (const char* p = SDL_GetAndroidInternalStoragePath(); p && *p) {
            setenv("HOME", p, 1);
        }
    }
}

void shutdown() {
    SDL_RemoveEventWatch(lifecycle_watch, nullptr);
}

bool take_lifecycle_resume() {
    return s_resume.exchange(false);
}

} // namespace oa::plat
