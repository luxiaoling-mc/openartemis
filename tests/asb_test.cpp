// ASB decoder tests: synthetic fixture + real fpm system scripts (env-gated).
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/fs/physfs_fs.h"
#include "core/runtime/runtime_iet.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
void put_str(std::vector<uint8_t>& out, const std::string& s) {
    put_u32(out, (uint32_t)s.size());
    out.insert(out.end(), s.begin(), s.end());
    out.push_back(0);
}

void test_synthetic() {
    std::vector<uint8_t> b;
    b.insert(b.end(), {'A', 'S', 'B', 0});
    b.push_back(0);
    put_u32(b, 4);
    // label main
    put_u32(b, 1);
    put_str(b, "main");
    // instruction jump label="start"
    put_u32(b, 0);
    put_str(b, "jump");
    put_u32(b, 0); // serial
    put_u32(b, 1); // params
    put_str(b, "label");
    put_str(b, "start");
    // label start
    put_u32(b, 1);
    put_str(b, "start");
    // instruction return
    put_u32(b, 0);
    put_str(b, "return");
    put_u32(b, 0);
    put_u32(b, 0);

    const std::string text = oa::runtime::decode_asb(b);
    check(text == "*main\n[jump label=\"start\"]\n*start\n[return]\n", "synthetic decode");
    // decoded text must round-trip through the regular parser
    const auto s = oa::runtime::Script::parse("decoded", text);
    check(s.labels.count("main") == 1 && s.labels.count("start") == 1, "labels parsed");
    check(s.instructions.size() == 2 && s.instructions[0].tag == "jump", "instructions parsed");
    check(s.instructions[0].get_or("label", "") == "start", "param parsed");
}

void test_real_fpm() {
    const char* pfs_path = std::getenv("OA_TEST_FPM_PFS");
    if (!pfs_path || !*pfs_path) return;
        oa::fs::PhysFileSystem fs(pfs_path, false);
    // script.asb must decode to the documented glue structure
    auto bytes = fs.read("system/script.asb");
    check(bytes.has_value(), "script.asb readable");
    if (!bytes) return;
    const std::string text = oa::runtime::decode_asb(*bytes);
    const auto s = oa::runtime::Script::parse("system/script.asb", text);
    check(s.labels.count("main") == 1 && s.labels.count("click") == 1 &&
              s.labels.count("click2") == 1 && s.labels.count("delay1") == 1 &&
              s.labels.count("popfunc01") == 1,
          "script.asb labels main/click/click2/delay/popfunc");
    int mainloop = 0, mainadd = 0;
    for (const auto& i : s.instructions) {
        const std::string* f = i.get("function");
        if (f && *f == "scriptMainloop") ++mainloop;
        if (f && *f == "scriptMainAdd") ++mainadd;
    }
    check(mainloop >= 10 && mainadd >= 10 && mainadd >= mainloop,
              "script.asb mainloop/mainadd pairs");
    // ui.asb decodes cleanly
    auto ui = fs.read("system/ui.asb");
    check(ui.has_value(), "ui.asb readable");
    if (ui) {
        const std::string t = oa::runtime::decode_asb(*ui);
        const auto us = oa::runtime::Script::parse("system/ui.asb", t);
        check(us.instructions.size() > 50, "ui.asb decodes to >50 instructions");
    }
    // save.asb decodes cleanly with *save/*last
    auto save = fs.read("system/save.asb");
    check(save.has_value(), "save.asb readable");
    if (save) {
        const std::string t = oa::runtime::decode_asb(*save);
        const auto ss = oa::runtime::Script::parse("system/save.asb", t);
        check(ss.labels.count("save") == 1 && ss.labels.count("last") == 1,
              "save.asb labels");
    }
    std::printf("asb: script.asb mainloop=%d mainadd=%d\n", mainloop, mainadd);
}

} // namespace

int main() {
    test_synthetic();
    test_real_fpm();
    if (failures) {
        std::fprintf(stderr, "asb_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("asb_test: all ok\n");
    return 0;
}
