// U15 regression: save-area image read-back (slot thumbnails) must go
// through the runtime's SaveStore, not a host-directory re-read that only
// honored OA_SAVE_ROOT. After U14 (commit 0855089) a plain run defaults the
// save store to ./save, so [savess] thumbnails were written but the save/load
// UI could never resolve them again (no env var => overlay root empty =>
// slots showed no thumbnail). The renderer now reads back through the very
// store the writes used, for every host (env override / default root /
// in-memory test stores). See docs/research/31-save-thumb-readback.md.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/fs/fs.h"
#include "core/render/renderer.h"
#include "core/runtime/runtime.h"
#include "core/media/image.h"
#include "core/runtime/runtime_save.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

struct MemFs : oa::fs::IFileSystem {
    std::map<std::string, std::string> files;
    std::optional<std::vector<uint8_t>> read(std::string_view path) const override {
        const auto it = files.find(std::string(path));
        if (it == files.end()) return std::nullopt;
        return std::vector<uint8_t>(it->second.begin(), it->second.end());
    }
    bool exists(std::string_view path) const override {
        return files.count(std::string(path)) > 0;
    }
    const char* kind() const override { return "mem"; }
};

class MemSaveStore final : public oa::runtime::SaveStore {
public:
    std::map<std::string, std::vector<uint8_t>> files;
    bool write(const std::string& rel, const std::vector<uint8_t>& d) override {
        files[rel] = d;
        return true;
    }
    std::optional<std::vector<uint8_t>> read(const std::string& rel) const override {
        const auto it = files.find(rel);
        if (it == files.end()) return std::nullopt;
        return it->second;
    }
    bool remove(const std::string& rel) override {
        return files.erase(rel) > 0;
    }
    bool exists(const std::string& rel) const override {
        return files.count(rel) > 0;
    }
};

std::vector<uint8_t> two_px_png() {
    // 2x1: left half red, right half blue (same fixture as misc-b W4).
    std::vector<uint8_t> rgba(8);
    rgba[0] = 255; rgba[1] = 0; rgba[2] = 0; rgba[3] = 255;
    rgba[4] = 0; rgba[5] = 0; rgba[6] = 255; rgba[7] = 255;
    return oa::media::encode_png(2, 1, rgba);
}

// The regression scenario: a *plain* host run — no OA_SAVE_ROOT — whose save
// store (default ./save or any injected store) holds a [savess] thumbnail.
void test_thumbnail_readback_through_store() {
    // The old code read back only the OA_SAVE_ROOT directory; clear the var
    // so this test would fail against that implementation.
#ifdef _WIN32
    _putenv_s("OA_SAVE_ROOT", "");
#else
    unsetenv("OA_SAVE_ROOT");
#endif

    auto fs = std::make_shared<MemFs>();
    fs->files["system.ini"] =
        "[WINDOWS]\nWIDTH = 1280\nHEIGHT = 720\nFPS = 60\nCHARSET = UTF-8\n"
        "BOOT = system/first.iet\nSAVEPATH = save\n";
    fs->files["system/first.iet"] = std::string("*main\n[stop]\n");
    // Asset-side image the fallback must still reach.
    const std::vector<uint8_t> asset_png = two_px_png();
    fs->files["image/a.png"] = std::string(asset_png.begin(), asset_png.end());

    auto store = std::make_shared<MemSaveStore>();
    store->write("save/thumb.png", two_px_png()); // what [savess] wrote

    oa::runtime::GameRuntime rt(fs);
    rt.set_save_store(store);
    rt.open_project("windows");
    rt.boot_project();

    oa::render::RenderEngine eng(fs.get(), &rt);

    // T1: the exact file [savess] wrote ("save/thumb.png" under the store,
    // SAVEPATH=save here) must resolve without OA_SAVE_ROOT set. The old
    // implementation only re-read the OA_SAVE_ROOT directory -> nullopt.
    const auto png = eng.overlay_read("save/thumb.png");
    check(png.has_value(), "U15 overlay_read resolves save-area file without OA_SAVE_ROOT");
    if (png) check(png->size() == asset_png.size(), "U15 overlay_read returns the PNG bytes");

    // T2: full image path used by the layer draw (extension fallback + PNG
    // decode), cached under the logical name.
    const oa::media::Image* img = eng.resolve_image("save/thumb");
    check(img != nullptr && img->w == 2 && img->h == 1,
          "U15 resolve_image decodes the save-area thumbnail (save/thumb -> .png)");
    if (img) {
        const uint8_t* px = img->rgba.data();
        check(px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255 &&
                  px[4] == 0 && px[5] == 0 && px[6] == 255 && px[7] == 255,
              "U15 thumbnail pixels decoded (left red, right blue)");
    }

    // T3: only names under the logical savepath may hit the store; the
    // overlay must not expose arbitrary store keys.
    store->write("keep/secret.png", two_px_png());
    check(!eng.overlay_read("keep/secret").has_value(),
          "U15 overlay_read refuses names outside the savepath");

    // T4: plain asset images still resolve through the project filesystem.
    const oa::media::Image* asset = eng.resolve_image("image/a");
    check(asset != nullptr && asset->w == 2 && asset->h == 1,
          "U15 asset image still resolves through the project fs");
}

} // namespace

int main() {
    test_thumbnail_readback_through_store();
    if (failures) {
        std::fprintf(stderr, "save_thumb_readback: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("save_thumb_readback: all checks passed\n");
    return 0;
}
