// Tests for config/ini, util/charset, project layer (M2).
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "core/fs/project.h"
#include "core/fs/physfs_fs.h"
#include "core/version.h"
#include "core/util/charset.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void test_charset() {
    check(oa::util::is_valid_utf8("abc\xe4\xb8\xad"), "utf8 valid cjk");
    check(!oa::util::is_valid_utf8("\xff\xfe"), "utf8 invalid");
    check(!oa::util::is_valid_utf8("\xc0\xaf"), "utf8 overlong rejected");
    const std::string_view bom = "\xEF\xBB\xBFhello";
    check(oa::util::strip_bom(bom) == "hello", "strip bom");
    check(oa::util::decode_to_utf8("hi \xe6\x97\xa5", "UTF-8") == "hi \xe6\x97\xa5",
          "decode utf8 passthrough");
}

void test_ini_parse() {
    const char* text =
        "; comment\n"
        "# another\n"
        "[WINDOWS]\n"
        "WIDTH = 1280\n"
        "height=720 ; inline comment\n"
        "RESIZABLE = true\n"
        "QUOTED = \"a;b\"\n"
        "EMPTY=\n"
        "[ANDROID]\n"
        "WIDTH = 960\n";
    std::string err;
    auto ini = oa::fs::IniFile::parse(text, &err);
    check(err.empty(), "ini parse ok");
    check(ini.sections.size() == 2, "ini two sections");
    const auto* win = ini.section("windows");
    check(win != nullptr, "section lookup case-insensitive");
    check(win && win->find("WIDTH") && *win->find("WIDTH") == "1280", "width parsed");
    check(oa::fs::ini_get(ini, "WINDOWS", "height") == "720", "lowercase key stored upper");
    check(oa::fs::ini_get(ini, "WINDOWS", "resizable") == "true", "bool text kept");
    check(oa::fs::ini_get(ini, "WINDOWS", "quoted") == "a;b", "quoted value kept");
    check(oa::fs::ini_get(ini, "WINDOWS", "empty") == "", "empty value");
    check(oa::fs::parse_bool("TRUE") && oa::fs::parse_bool("1") &&
              oa::fs::parse_bool("yes") && oa::fs::parse_bool("On"),
          "bool true forms");
    check(!oa::fs::parse_bool("0") && !oa::fs::parse_bool("no") &&
              !oa::fs::parse_bool(""),
          "bool false forms");
    check(oa::fs::ini_get(ini, "NOPE", "k", "d") == "d", "missing section fallback");
}

void test_project_synthetic() {
    // In-memory FS with a minimal system.ini
    struct MemFs : oa::fs::IFileSystem {
        std::string content;
        std::optional<std::vector<uint8_t>> read(std::string_view p) const override {
            if (p == "system.ini") {
                return std::vector<uint8_t>(content.begin(), content.end());
            }
            return std::nullopt;
        }
        const char* kind() const override { return "mem"; }
    };
    MemFs fs;
    fs.content = "[WINDOWS]\r\nWIDTH = 1920\r\nHEIGHT = 1080\r\nFPS = 60\r\n"
                 "CHARSET = UTF-8\r\nBOOT = system/first.iet\r\nSAVEPATH = save\\cn\r\n"
                 "RESIZABLE = 1\r\nFIXED_ASPECT_RATIO = 1\r\nSIDECUT = 1\r\n";
    auto p = oa::fs::Project::open(fs, "windows");
    check(p.config.stage_width == 1920 && p.config.stage_height == 1080, "stage size");
    check(p.config.boot_script == "system/first.iet", "boot script");
    check(p.config.savepath == "save\\cn", "savepath raw kept");
    check(p.config.charset == "UTF-8", "charset");
    check(p.config.resizable && p.config.fixed_aspect_ratio && p.config.sidecut,
          "flag bools");
    check(p.config.env.at("FPS") == "60", "env keeps fps");
    bool threw = false;
    try {
        oa::fs::Project::open(fs, "android");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "missing platform section throws");
}

void test_project_real_fpm() {
    const char* pfs_path = std::getenv("OA_TEST_FPM_PFS");
    if (!pfs_path || !*pfs_path) return; // silently skip when env absent
    oa::fs::PhysFileSystem fs(pfs_path, false);
    const auto p = oa::fs::Project::open(fs, "windows");
    check(p.config.stage_width == 1280 && p.config.stage_height == 720, "fpm 1280x720");
    check(p.config.fps == 60, "fpm fps 60");
    check(p.config.boot_script == "system/first.iet", "fpm boot");
    check(p.config.charset == "UTF-8", "fpm charset utf8");
    check(p.config.savepath == "savedata_cn", "fpm savepath");
    check(p.config.fixed_aspect_ratio && p.config.resizable, "fpm flags");
    check(!p.config.sidecut, "fpm sidecut off");
    check(p.config.platform == "windows", "platform lowercase");
    std::printf("fpm project: %dx%d fps=%d boot=%s charset=%s savepath=%s\n",
                p.config.stage_width, p.config.stage_height, p.config.fps,
                p.config.boot_script.c_str(), p.config.charset.c_str(),
                p.config.savepath.c_str());
}

} // namespace

int main() {
    oa::log_build_info();
    test_charset();
    test_ini_parse();
    test_project_synthetic();
    test_project_real_fpm();
    if (failures) {
        std::fprintf(stderr, "ini_project_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("ini_project_test: all ok\n");
    return 0;
}
