// pfs_real_test — validates the C++ reader against the real fpm/root.pfs
// archive using precomputed SHA-256 fingerprints (from tools/pfs.py).
// Skipped (exit 77) when OA_TEST_FPM_PFS is unset.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "sha256_mini.h"
#include "core/fs/physfs_fs.h"

namespace {

struct Sample {
    const char* name;
    const char* sha256;
};

const Sample kSamples[] = {
    {"system.ini", "a4fd3b68a72864384b3526da61ba8c8a9ece89b9a062e4d5c2d4481a9f7cee14"},
    {"system/first.iet", "5c4e2ceb27264c27602e0f3e16afb4b79c40dd8bcc74ea1372707cb330dfdaf8"},
    {"system/script.asb", "c97e393645c162034d96492e9286fca95dfa6780710ec495554e1215c3375cfc"},
    {"system/ui.asb", "cd5b945bf4ae7978327f9e11be9d46ea1113f2191353ab3558ac26962f88fa0b"},
    {"system/init.lua", "5573fd4d0cdf69aaa57aa8a17452f40ed8a66a4aa7aebe6c9c9dcc6de99a7291"},
    {"script/brandlogo.ast", "7aa4e75c750325052042969e9d54050bd50012e8010b56ec4fdc65a7bd49158c"},
    {"script/pack.ast", "69452db4b52014ac99bf6a6c624967d9b1e3d157679d950a016c6b7194c578b9"},
    {"script/gamestart.ast", "887c97f0492f6c3e7e7ff3eb14e454300f71e430c027ed9011549bc05f7c9296"},
    {"font/sourcehansans-medium.otf", "0a2ed7c3978791c654b98470477c494763a7f86f8231e738aad725be9c2a6e74"},
    {"font/extra_28.rft", "0e2c80cf65687daf2cfbcde4226798e6f422d1d5d2b7006e467f8ad4ad6e920a"},
    {"image/bg/zbg001_a.png", "df289ed1ef30ceee23c996dbb913ef247681f961e55e613080f50677ba21b147"},
    {"image/anime/line21.png", "27a1f019a14ebf5a2cbdaad50fe7db9c00fbdbafec911971f47e20a5053b400b"},
    {"image/anime/line2.ipt", "df49532696f06611cbf753e7395b51a6927364ece1582a61f2ca13720dbd32d7"},
    {"sound/bgm/bgm01_a.ogg", "b57f51a225f5ee3d3fca0006cf95bcf2e05c30a85a07cb4537b7af58ef6a6b67"},
    {"sound/vo/ame/fem_ame_00001.ogg", "7fb5cbb658d11695a0786ac8b3d0d6429fa2b49abc7d54617c36e1d7c29e8f0f"},
    {"movie/logo.ogv", "bfc481e358b1add8c5f8a2814111479e330174b083cb9225eb2460aae6daca07"},
    {"pc/ja/title/bg.png", "5f53e94189b6ae9dab289eaff68475c553045a191b0f93b04edd7b8d979ecc7e"},
    {"pc/ja/mw/select.png", "28b249faaf97115d5547d66ae99a5960f3ef8468489be95a85052bef0a805784"},
};

} // namespace

int main() {
    const char* pfs_path = std::getenv("OA_TEST_FPM_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_FPM_PFS not set; skipping\n");
        return 77;
    }
    const oa::fs::PfsInfo pfs = oa::fs::scan_pfs_file(pfs_path);
    if (pfs.version != '6') {
        std::fprintf(stderr, "expected pf6 archive\n");
        return 1;
    }
    if (pfs.entries.size() != 29237) {
        std::fprintf(stderr, "unexpected entry count %zu\n", pfs.entries.size());
        return 1;
    }
    if (pfs.index_size != 1576967) {
        std::fprintf(stderr, "unexpected index_size %u\n", pfs.index_size);
        return 1;
    }
    oa::fs::PhysFileSystem fs(pfs_path, false);
    int bad = 0;
    for (const auto& s : kSamples) {
        const std::optional<std::vector<uint8_t>> data_opt = fs.read(s.name);
        if (!data_opt) {
            std::fprintf(stderr, "MISSING: %s\n", s.name);
            ++bad;
            continue;
        }
        const std::vector<uint8_t> data = *data_opt;
        const std::string got = test_util::sha256_hex(data);
        if (got != s.sha256) {
            std::fprintf(stderr, "HASH MISMATCH: %s\n  want %s\n  got  %s\n", s.name, s.sha256,
                         got.c_str());
            ++bad;
        } else {
            std::printf("ok %-38s %zu bytes\n", s.name, data.size());
        }
        // chunked read must agree with whole read (decryption phase resume)
        std::vector<uint8_t> chunk(data.size());
        size_t done = 0;
        bool ok = true;
        while (done < data.size()) {
            const size_t step = (done % 31) + 1; // awkward strides
            const size_t want = std::min(step, data.size() - done);
            const std::optional<std::vector<uint8_t>> part =
                fs.read_range(s.name, done, want);
            if (!part || part->size() != want) {
                ok = false;
                break;
            }
            std::memcpy(chunk.data() + done, part->data(), want);
            done += want;
        }
        if (!ok || chunk != data) {
            std::fprintf(stderr, "CHUNKED MISMATCH: %s\n", s.name);
            ++bad;
        }
    }
    // case/separator-insensitive lookup spot check
    if (!fs.exists("SYSTEM.INI") || !fs.exists("system\\first.iet") ||
        !fs.exists("Image\\Bg\\Zbg001_a.png")) {
        std::fprintf(stderr, "normalized lookup failed\n");
        ++bad;
    }
    if (bad) {
        std::fprintf(stderr, "pfs_real_test: %d failure(s)\n", bad);
        return 1;
    }
    std::printf("pfs_real_test: all %d samples ok\n",
                int(sizeof(kSamples) / sizeof(kSamples[0])));
    return 0;
}