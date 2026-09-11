// P3b text-domain tests (see docs/research/15-text2.md): reveal/scetween
// timing (U1-U4), click-to-reveal gating (U5), backlog archive + reproduction
// tags + rp override (U6-U8), message_tags reproduction (U9), glyph icon
// config & click-wait placement (U10), style/raw font keys (U11).
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/fs/fs.h"
#include "core/runtime/runtime.h"
#include "core/render/text.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using oa::render::TextEngine;

std::map<std::string, std::string> tag(
    const std::initializer_list<std::pair<const char*, const char*>>& kv) {
    std::map<std::string, std::string> m;
    for (const auto& [k, v] : kv) m[k] = v;
    return m;
}

// scetween: type=in delay=25（每字 25ms）
void test_reveal_timing() {
    TextEngine e;
    e.apply_scetween(tag({{"mode", "init"}, {"type", "in"}, {"delay", "25"},
                          {"time", "0"}}));
    e.push_text("あいう"); // 3 字
    {
        const oa::render::MessageLayer* l = e.layer("adv01");
        check(l && l->reveal_pending, "U1 new text is reveal pending");
        check(l && l->char_count == 3, "U1 char count = 3");
    }
    e.reveal_next(0);
    check(e.layer("adv01")->reveal_index == 1, "U1 index starts at 1");
    e.reveal_next(24);
    check(e.layer("adv01")->reveal_index == 1, "U2 before a delay slot stays 1");
    e.reveal_next(1); // 25ms -> slot 2
    check(e.layer("adv01")->reveal_index == 2, "U2 after one delay slot -> 2");
    e.reveal_next(50); // clock 75 -> 4th slot, clamped to 3; total=75 -> done
    check(e.layer("adv01")->reveal_index == 3, "U3 index clamps at char count");
    check(!e.layer("adv01")->reveal_pending, "U3 pending cleared at max_total");
    check(e.is_reveal_complete(), "U3 reveal complete");
    // 无 scetween：首帧全量
    TextEngine e2;
    e2.push_text("xy");
    check(e2.layer("adv01")->reveal_pending, "U4 plain text pending");
    e2.reveal_next(1);
    check(e2.layer("adv01")->reveal_index == 2 && !e2.layer("adv01")->reveal_pending,
          "U4 no scetween => full reveal on the first advance");
    // reveal_all 强制整页
    TextEngine e3;
    e3.apply_scetween(tag({{"type", "in"}, {"delay", "100"}}));
    e3.push_text("abcdef");
    e3.reveal_all();
    check(e3.layer("adv01")->reveal_index == 6 && e3.is_reveal_complete(),
          "U4 reveal_all finishes the page");
}

// runtime：Generic 点击等待 + 逐字未完成 → 首击整页揭示、二击推进
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

void test_runtime_click_reveal_gate() {
    MemFs fs;
    fs.files["system.ini"] =
        "[WINDOWS]\nWIDTH = 1280\nHEIGHT = 720\nFPS = 60\nCHARSET = UTF-8\n"
        "BOOT = system/first.iet\nSAVEPATH = save\n";
    // scetween 每字 10000ms → 揭示远未完成即停在 [@]
    fs.files["system/first.iet"] =
        "*main\n"
        "[chgmsg id=\"adv\"]\n"
        "[font face=\"font/sourcehansans-medium.otf\"]\n"
        "[scetween mode=\"init\" type=\"in\" delay=\"10000\"]\n"
        "[print data=\"こんにちは\"]\n"
        "[@]\n"
        "[chgmsg_close]\n"
        "[stop]\n";
    oa::runtime::GameRuntime rt(std::make_shared<MemFs>(fs));
    rt.open_project("windows");
    rt.boot_project();
    oa::runtime::FrameInput idle;
    rt.tick(16, idle);
    (void)rt.drain_events();
    const oa::runtime::WaitReason* w = rt.current_wait();
    check(w && w->kind == oa::runtime::WaitReason::Kind::Generic,
          "U5 script parks at [@]");
    check(!rt.text().is_reveal_complete(), "U5 reveal still pending");
    // 首击：只整页揭示，不离开等待
    oa::runtime::FrameInput click;
    click.left_click_edge = true;
    rt.tick(16, click);
    (void)rt.drain_events();
    check(rt.text().is_reveal_complete(), "U5 first click reveals the page");
    check(rt.current_wait() && rt.current_wait()->kind ==
                                   oa::runtime::WaitReason::Kind::Generic,
          "U5 first click does not leave the wait");
    // 二击：推进（/chgmsg + [stop]）
    rt.tick(16, click);
    (void)rt.drain_events();
    rt.tick(16, idle);
    (void)rt.drain_events();
    w = rt.current_wait();
    check(w && w->kind == oa::runtime::WaitReason::Kind::Stop,
          "U5 second click advances to [stop]");
}

void test_backlog_archive() {
    // writebacklog mode=1：换页入历史（fontdefault 先定默认字体→页首快照）
    TextEngine e;
    e.set_backlog_write_mode(true);
    e.font_default(tag({{"size", "40"}}));
    e.push_text("こんにちは");
    e.apply_font(tag({{"color", "FF0000"}}));
    e.line_break(true);
    e.push_text("次");
    check(e.backlog_size() == 0, "U6 nothing stored before rp");
    e.page_break(std::nullopt); // 缺省按 write_mode
    check(e.backlog_size() == 1, "U6 rp archived the page (write_mode)");
    auto tags = e.backlog_tags(0, false);
    check(tags.has_value(), "U6 page 0 tags available");
    if (tags) {
        check((*tags)[0] == "[print data=\"こんにちは\"]", "U6 text tag first");
        check((*tags)[1] == "[font color=\"FF0000\"]", "U6 font tag kept");
        check((*tags)[2] == "[rt]", "U6 rt tag kept");
        check((*tags)[3] == "[print data=\"次\"]", "U6 second text tag");
    }
    auto allfont = e.backlog_tags(0, true);
    check(allfont && allfont->size() == 5 &&
              (*allfont)[0].rfind("[font ", 0) == 0 &&
              (*allfont)[0].find("size=\"40\"") != std::string::npos,
          "U6 allfont prepends the page-start font");
    // rp backlog=0/1 参数无视 write_mode
    TextEngine e2;
    e2.set_backlog_write_mode(true);
    e2.push_text("x");
    e2.page_break(0);
    check(e2.backlog_size() == 0, "U7 rp backlog=0 overrides write_mode");
    e2.push_text("y");
    e2.page_break(1);
    check(e2.backlog_size() == 1, "U7 rp backlog=1 stores");
    // allow=0 不入库；includefont=0 剥字体
    TextEngine e3;
    e3.set_backlog_write_mode(true);
    e3.apply_backlog_config(tag({{"allow", "0"}}), false);
    e3.push_text("a");
    e3.page_break(std::nullopt);
    check(e3.backlog_size() == 0, "U7 backlog allow=0 drops the page");
    TextEngine e4;
    e4.set_backlog_write_mode(true);
    e4.apply_backlog_config(tag({{"includefont", "0"}}), false);
    e4.apply_font(tag({{"size", "40"}}));
    e4.push_text("b");
    e4.page_break(std::nullopt);
    auto t4 = e4.backlog_tags(0, false);
    check(t4 && t4->size() == 1 && (*t4)[0] == "[print data=\"b\"]",
          "U7 includefont=0 strips font tags");
    // clear=1
    e4.apply_backlog_config(tag({}), true);
    check(e4.backlog_size() == 0, "U7 backlog clear empties history");
}

void test_message_tags() {
    TextEngine e;
    e.font_default(tag({{"size", "40"}}));
    e.push_text("あい");
    e.line_break(false);
    e.apply_font(tag({{"color", "FF0000"}}));
    e.push_text("うえ");
    auto tags = e.message_tags("adv01", false);
    check(tags.has_value(), "U8 message tags available");
    if (tags) {
        check((*tags)[0] == "[print data=\"あい\"]", "U8 text first");
        check((*tags)[1] == "[rt]", "U8 rt present");
        check((*tags)[2] == "[font color=\"FF0000\"]", "U8 font tag present");
        check((*tags)[3] == "[print data=\"うえ\"]", "U8 second text");
    }
    auto allfont = e.message_tags("adv01", true);
    check(allfont && !allfont->empty() &&
              (*allfont)[0].rfind("[font ", 0) == 0 &&
              (*allfont)[0].find("size=\"40\"") != std::string::npos,
          "U8 allfont prepends page-start font");
    check(!e.message_tags("nope", false).has_value(), "U8 unknown layer -> none");
    e.page_break(std::nullopt);
    auto after = e.message_tags("adv01", false);
    check(after && after->empty(), "U8 after rp the page tags are cleared");
}

void test_glyph_icon_and_placement() {
    TextEngine e;
    e.apply_font(tag({{"left", "100"}, {"top", "300"}, {"width", "400"}}));
    e.push_text("あ");
    e.set_glyph_config(tag({{"layer", "icon"}, {"left", "8"}, {"top", "4"}}));
    const oa::render::GlyphIconConfig& g = e.glyph_icon();
    check(g.layer == "icon" && g.left == 8 && g.top == 4, "U9 glyph config parsed");
    auto p = e.click_wait_placement(false);
    check(p.has_value() && p->layer_id == "icon" && p->left >= 500 && p->top == 304,
          "U9 line-end placement uses text-area end + offset");
    // 两处都缺省 → 无图标
    TextEngine e2;
    e2.push_text("x");
    check(!e2.click_wait_placement(false).has_value(), "U9 no config => no icon");
}

void test_click_wait_icon_driving() {
    // misc-A: [glyph] click-wait icon 宿主驱动（ advance_click_wait
    // + runtime/ enter/exit_click_wait_icon）。Generic 点击等待且文本揭示
    // 完成 → 图标层显示（homing 写摆放坐标）；离开等待/揭示未完成 → 隐藏。
    {
        // A: 瞬时揭示 → 停 [@] 首帧即显；点击离开 → 隐藏。
        MemFs fs;
        fs.files["system.ini"] =
            "[WINDOWS]\nWIDTH = 1280\nHEIGHT = 720\nFPS = 60\nCHARSET = UTF-8\n"
            "BOOT = system/first.iet\nSAVEPATH = save\n";
        fs.files["system/first.iet"] =
            "*main\n"
            "[chgmsg id=\"adv\"]\n"
            "[font face=\"font/sourcehansans-medium.otf\" left=\"100\" top=\"300\" "
            "width=\"400\"]\n"
            "[glyph layer=\"icon\" left=\"8\" top=\"4\" homing=\"1\"]\n"
            "[print data=\"こんにちは\"]\n"
            "[@]\n"
            "[stop]\n";
        oa::runtime::GameRuntime rt(std::make_shared<MemFs>(fs));
        rt.open_project("windows");
        rt.boot_project();
        oa::runtime::FrameInput idle;
        rt.tick(16, idle);
        (void)rt.drain_events();
        const oa::render::Layer* ic = rt.scene().find("icon");
        check(ic && ic->visible, "U12 icon shown at Generic wait + reveal complete");
        check(ic && ic->left == 508 && ic->top == 304,
              "U12 homing writes placement (text-area end + glyph offset)");
        oa::runtime::FrameInput click;
        click.left_click_edge = true;
        rt.tick(16, click);
        (void)rt.drain_events();
        rt.tick(16, idle);
        (void)rt.drain_events();
        ic = rt.scene().find("icon");
        check(rt.current_wait() &&
                  rt.current_wait()->kind == oa::runtime::WaitReason::Kind::Stop,
              "U12 script parks at [stop] after the click wait");
        check(ic && !ic->visible, "U12 icon hidden when the click wait leaves");
    }
    {
        // B: 逐字未完成（delay 10000ms）→ 不显示；首击整页揭示 → 同帧显示；
        // 再击推进到 [stop] → 隐藏。
        MemFs fs;
        fs.files["system.ini"] =
            "[WINDOWS]\nWIDTH = 1280\nHEIGHT = 720\nFPS = 60\nCHARSET = UTF-8\n"
            "BOOT = system/first.iet\nSAVEPATH = save\n";
        fs.files["system/first.iet"] =
            "*main\n"
            "[chgmsg id=\"adv\"]\n"
            "[font face=\"font/sourcehansans-medium.otf\" left=\"100\" top=\"300\" "
            "width=\"400\"]\n"
            "[glyph layer=\"icon\" left=\"8\" top=\"4\" homing=\"1\"]\n"
            "[scetween mode=\"init\" type=\"in\" delay=\"10000\"]\n"
            "[print data=\"こんにちは\"]\n"
            "[@]\n"
            "[stop]\n";
        oa::runtime::GameRuntime rt(std::make_shared<MemFs>(fs));
        rt.open_project("windows");
        rt.boot_project();
        oa::runtime::FrameInput idle;
        rt.tick(16, idle);
        (void)rt.drain_events();
        const oa::render::Layer* ic = rt.scene().find("icon");
        check(rt.text().is_reveal_complete() == false, "U12 reveal still pending");
        check(!(ic && ic->visible), "U12 icon hidden while reveal incomplete");
        oa::runtime::FrameInput click;
        click.left_click_edge = true;
        // 首击：只整页揭示，等待仍在 → 图标出现
        rt.tick(16, click);
        (void)rt.drain_events();
        ic = rt.scene().find("icon");
        check(rt.text().is_reveal_complete(), "U12 first click reveals the page");
        check(ic && ic->visible, "U12 icon appears once reveal is complete");
        // 二击：推进 → [stop] → 隐藏
        rt.tick(16, click);
        (void)rt.drain_events();
        rt.tick(16, idle);
        (void)rt.drain_events();
        ic = rt.scene().find("icon");
        check(rt.current_wait() &&
                  rt.current_wait()->kind == oa::runtime::WaitReason::Kind::Stop,
              "U12 second click reaches [stop]");
        check(ic && !ic->visible, "U12 icon hidden after advancing");
    }
}

void test_style_keys_stored() {
    TextEngine e;
    e.apply_font(tag({{"color", "0FF0000"}, {"shadowcolor", "000080"},
                      {"outlinecolor", "FFFFFF"}, {"bold", "1"}}));
    const oa::render::FontDesc* f = &e.layer("adv01")->font;
    check(f->get("color") && *f->get("color") == "0FF0000", "U10 raw color kept");
    check(f->get("shadowcolor") && f->get("outlinecolor"), "U10 style keys kept raw");
    // 页面正文判定：空格垫层不算正文
    e.push_text("　");
    check(!e.page_has_visible_text("adv01"), "U10 full-width spaces are not body text");
    e.push_text("本");
    check(e.page_has_visible_text("adv01"), "U10 non-blank char is body text");
}

} // namespace

int main() {
    test_reveal_timing();
    test_runtime_click_reveal_gate();
    test_backlog_archive();
    test_message_tags();
    test_glyph_icon_and_placement();
    test_style_keys_stored();
    test_click_wait_icon_driving();
    if (failures) {
        std::fprintf(stderr, "text_reveal_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("text_reveal_test: all ok\n");
    return 0;
}
