#include "core/version.h"

#include <SDL3/SDL_version.h>
#include <cstdio>

namespace oa {
void log_build_info() {
    const int v = SDL_GetVersion();
    std::printf("[openartemis] version %s (SDL %d.%d.%d)\n", kVersion,
                SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v),
                SDL_VERSIONNUM_MICRO(v));
}
} // namespace oa
