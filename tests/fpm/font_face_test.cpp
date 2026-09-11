// P4U regression (research/46): the default text face must resolve on every
// project, not only on FPM. Root cause of "NekoMiko story text invisible":
// the engine default font name (font/sourcehansans-medium.otf) is an FPM
// asset; NekoMiko ships font/GenJyuuGothic-Bold.ttf instead and the face
// stayed null → zero glyphs drawn (headless buffer tests never rasterized
// text). FontSystem now falls back to the project's font/ listing (and a
// small explicit candidate list for backends without listing).
// This test instantiates FontSystem (freetype face load only, no SDL
// renderer needed) over the real archives and asserts a default face exists.
// Env: OA_TEST_FPM_PFS / OA_TEST_NEKOMIKO_PFS (arms skipped when unset;
// exit 77 when neither is set).
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "core/fs/physfs_fs.h"
#include "core/fs/physfs_fs.h"
#include "core/render/font.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* fpm = std::getenv("OA_TEST_FPM_PFS");
    const char* nm = std::getenv("OA_TEST_NEKOMIKO_PFS");
    if ((!fpm || !*fpm) && (!nm || !*nm)) {
        std::printf("neither archive env set; skipping\n");
        return 77;
    }
    if (fpm && *fpm) {
        try {
                        oa::fs::PhysFileSystem fs(fpm, false);
            oa::render::FontSystem font(&fs, nullptr);
            std::printf("[font] FPM default face: %s\n",
                        font.default_face_loaded() ? "loaded" : "MISSING");
            check(font.default_face_loaded(), "FPM: default text face resolves");
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "FAIL: FPM arm: %s\n", ex.what());
            ++failures;
        }
    }
    if (nm && *nm) {
        try {
                        oa::fs::PhysFileSystem fs(nm, false);
            oa::render::FontSystem font(&fs, nullptr);
            std::printf("[font] NekoMiko default face: %s\n",
                        font.default_face_loaded() ? "loaded" : "MISSING");
            check(font.default_face_loaded(),
                  "NekoMiko: default text face resolves (font/ fallback)");
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "FAIL: NekoMiko arm: %s\n", ex.what());
            ++failures;
        }
    }
    std::printf("%s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
