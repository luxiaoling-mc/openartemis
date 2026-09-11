// U11 folder-start acceptance (headless, real fpm/root.pfs, env-gated):
// extract a boot-self-consistent SUBSET (system.ini + system/ + font/ +
// pc/ + script/brandlogo.ast + sound/sysse/none.ogg — the set exploration
// recorded in research/25 §3) into a tmp directory, boot GameRuntime over
// the new DirFileSystem and drive 900 frames. The full end-of-run stat
// tuple must be byte-identical to the same drive over the PFS archive
// (layers=131 layer_ev=347 text_ev=2 switch_ev=35 msg_lines=2 handlers=43
// wait=stop media_ev=21 audio_players=1 bgm=1 se=0 voice=0), and the boot
// subset files must round-trip byte-exact.
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
struct StatTuple {
    size_t layers = 0;
    size_t layer_ev = 0;
    size_t text_ev = 0;
    size_t switch_ev = 0;
    size_t msg_lines = 0;
    size_t handlers = 0;
    int wait_stop = 0;
    size_t media_ev = 0;
    size_t audio_players = 0;
    int bgm = 0;
    size_t se = 0;
    size_t voice = 0;
};
StatTuple drive900(oa::runtime::GameRuntime& rt) {
    oa::runtime::FrameInput idle;
    for (size_t f = 0; f < 900; ++f) rt.tick(16, idle);
    StatTuple t;
    t.layers = rt.scene().size();
    t.layer_ev = rt.scene_layer_events();
    t.text_ev = rt.text_events();
    t.switch_ev = rt.message_switch_events();
    t.msg_lines = rt.text().visible_content_layers().size();
    for (const oa::render::Layer* l : rt.scene().draw_order())
        if (!l->event_handlers.empty()) ++t.handlers;
    const auto* w = rt.current_wait();
    t.wait_stop = w && w->kind == oa::runtime::WaitReason::Kind::Stop ? 1 : 0;
    t.media_ev = rt.media_events();
    t.audio_players = rt.media_players().active_players();
    t.bgm = rt.audio().is_bgm_playing() ? 1 : 0;
    t.se = rt.audio().state().se_channels.size();
    t.voice = rt.audio().state().voice_channels.size();
    return t;
}
bool same(const StatTuple& a, const StatTuple& b) {
    return a.layers == b.layers && a.layer_ev == b.layer_ev && a.text_ev == b.text_ev &&
           a.switch_ev == b.switch_ev && a.msg_lines == b.msg_lines &&
           a.handlers == b.handlers && a.wait_stop == b.wait_stop &&
           a.media_ev == b.media_ev && a.audio_players == b.audio_players &&
           a.bgm == b.bgm && a.se == b.se && a.voice == b.voice;
}
void remove_tree(const std::string& path) {
    // Windows port (research/128): `rm -rf` is POSIX-only; the std::filesystem
    // equivalent removes the same tree (and is a no-op when it is gone).
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}
/// The boot-self-consistent subset filter (research/25 §3).
bool subset_filter(const oa::fs::PfsEntryInfo& e) {
    std::string p;
    for (const char c : e.path) p.push_back(c == '\\' ? '/' : c);
    const size_t sep = p.find('/');
    const std::string top = sep == std::string::npos ? p : p.substr(0, sep);
    if (p == "system.ini") return true;
    if (top == "system" || top == "font" || top == "pc") return true;
    return p == "script/brandlogo.ast" || p == "sound/sysse/none.ogg";
}
} // namespace

int main() {
    const char* pfs_path = std::getenv("OA_TEST_FPM_PFS");
    if (!pfs_path || !*pfs_path) {
        std::printf("OA_TEST_FPM_PFS unset; skipping\n");
        return 77;
    }
    // Extract the subset into a fresh tmp dir.
#ifdef _WIN32
    // Windows port (research/128): the PFS index stores UTF-8 names, and the
    // engine's oa::fs::extract_pfs_archive() hands those bytes straight to
    // std::fopen. UCRT narrow file APIs decode char* with the ACTIVE ANSI code
    // page (936 on this machine), which rejects valid UTF-8 sequences with
    // EILSEQ — the same input that works on the POSIX build. Declaring this
    // process's C locale as UTF-8 makes the CRT decode char* paths exactly like
    // the POSIX build does (the ".UTF-8" locale is the C locale with UTF-8
    // encoding, so no numeric/formatting behaviour changes). Assertions below
    // are untouched; the engine-side fix (`opfs extract` still fails the same
    // way, repro in research/128 §4) is a separate follow-up.
    check(std::setlocale(LC_ALL, ".UTF-8") != nullptr,
          "UTF-8 C locale available (extraction writes UTF-8 entry names)");
#endif
    // Windows port (research/128): mkdtemp("/tmp/...") is POSIX-only — use the
    // system temp dir + pid (the same shape as tests/fpm/p3_select_test.cpp and
    // save_load_ui_test.cpp) so the folder-start boot runs on MSVC too. The
    // directory is asserted to really exist (the old `dir != nullptr` gate).
    namespace fsf = std::filesystem;
    std::error_code ec;
    const fsf::path dir_path =
        fsf::temp_directory_path(ec) / ("oa_folders_" + std::to_string(::getpid()));
    fsf::remove_all(dir_path, ec); // fresh tree, like mkdtemp's guarantee
    fsf::create_directories(dir_path, ec);
    check(fsf::is_directory(dir_path, ec), "temp dir created");
    const std::string dir_str = dir_path.string();
    size_t extracted = 0;
    try {
        extracted = oa::fs::extract_pfs_archive(pfs_path, dir_str, subset_filter);
        // Round-trip byte checks: extracted files == archive bytes.
        oa::fs::PhysFileSystem arc_fs(pfs_path, false);
        for (const char* n : {"system.ini", "system/ui.asb", "system/first.iet",
                              "font/sourcehansans-medium.otf", "pc/ja/btn_title.png"}) {
            const std::optional<std::vector<uint8_t>> arc = arc_fs.read(n);
            if (!arc) continue; // subset member may vary by name; only check hits
            std::string real = dir_str;
            real += "/";
            for (const char* q = n; *q; ++q) real.push_back(*q == '/' ? '/' : *q);
            std::string disk;
            if (std::FILE* f = std::fopen(real.c_str(), "rb")) {
                std::vector<uint8_t> buf;
                char rb[4096];
                size_t r;
                while ((r = std::fread(rb, 1, sizeof rb, f)) > 0) disk.append(rb, r);
                std::fclose(f);
                check(disk.size() == arc->size() &&
                          std::memcmp(disk.data(), arc->data(), arc->size()) == 0,
                      ("round-trip byte-exact: " + std::string(n)).c_str());
            } else {
                check(false, ("extracted file readable: " + std::string(n)).c_str());
            }
        }
    } catch (const std::exception& e) {
        check(false, e.what());
    }
    check(extracted > 300, "subset extraction wrote the boot set");    // Dir boot.
    StatTuple dir_stat{};
    try {
        auto dfs = std::make_shared<oa::fs::PhysFileSystem>(dir_str);
        oa::runtime::GameRuntime rt(dfs);
        rt.open_project("windows");
        rt.boot_project();
        dir_stat = drive900(rt);
    } catch (const std::exception& e) {
        check(false, e.what());
    }
    // PFS boot (baseline).
    StatTuple pfs_stat{};
    try {
        auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path, false);
        oa::runtime::GameRuntime rt(fs);
        rt.open_project("windows");
        rt.boot_project();
        pfs_stat = drive900(rt);
    } catch (const std::exception& e) {
        check(false, e.what());
    }
    std::printf("[fs] dir  : layers=%zu layer_ev=%zu text_ev=%zu switch_ev=%zu "
                "msg_lines=%zu handlers=%zu wait_stop=%d media_ev=%zu players=%zu "
                "bgm=%d se=%zu vo=%zu\n",
                dir_stat.layers, dir_stat.layer_ev, dir_stat.text_ev, dir_stat.switch_ev,
                dir_stat.msg_lines, dir_stat.handlers, dir_stat.wait_stop,
                dir_stat.media_ev, dir_stat.audio_players, dir_stat.bgm, dir_stat.se,
                dir_stat.voice);
    std::printf("[fs] pfs  : layers=%zu layer_ev=%zu text_ev=%zu switch_ev=%zu "
                "msg_lines=%zu handlers=%zu wait_stop=%d media_ev=%zu players=%zu "
                "bgm=%d se=%zu vo=%zu\n",
                pfs_stat.layers, pfs_stat.layer_ev, pfs_stat.text_ev, pfs_stat.switch_ev,
                pfs_stat.msg_lines, pfs_stat.handlers, pfs_stat.wait_stop,
                pfs_stat.media_ev, pfs_stat.audio_players, pfs_stat.bgm, pfs_stat.se,
                pfs_stat.voice);
    check(same(dir_stat, pfs_stat),
          "folder-start 900-frame stat tuple identical to the PFS boot");
    check(dir_stat.layers == 131 && dir_stat.layer_ev == 347 && dir_stat.wait_stop == 1,
          "folder-start stat matches the recorded baseline (131/347/stop)");
    // The extracted dir must still boot a second time (fresh runtime) — the
    // tree is a stable project source, not a one-shot staging area.
    try {
        auto dfs = std::make_shared<oa::fs::PhysFileSystem>(dir_str);
        oa::runtime::GameRuntime rt(dfs);
        rt.open_project("windows");
        rt.boot_project();
        const StatTuple again = drive900(rt);
        check(same(again, dir_stat), "second folder boot is stable (same tuple)");
    } catch (const std::exception& e) {
        check(false, e.what());
    }
    remove_tree(dir_str);
    if (failures) {
        std::fprintf(stderr, "folder_start_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("folder_start_test: all ok\n");
    return 0;
}
