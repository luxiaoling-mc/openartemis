// misc-A backlog var query bridge tests (docs/research/19-misc-a.md §V):
//   - V1-V3 interpreter-level: e:var system=get_backlog_size/get_backlog_tags/
//     get_message_tags bridge semantics (size scalar,
//     pseudo array name.0..N-1 + name.size, out-of-range → size=0 only,
//     no hook → conservative fallback).
//   - V4 engine-integration: the same hooks wired to a real oa::render::TextEngine
//     (the runtime wiring shape in runtime.cpp open_project) return the actual
//     P3b backlog / message-tag data.
// Headless, no FPM archive needed.
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/runtime/runtime_iet.h"
#include "core/render/text.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using oa::runtime::Interpreter;
using oa::runtime::ValueKind;
using oa::render::TextEngine;

std::map<std::string, std::string> tag(
    const std::initializer_list<std::pair<const char*, const char*>>& kv) {
    std::map<std::string, std::string> m;
    for (const auto& [k, v] : kv) m[k] = v;
    return m;
}

int64_t var_int(Interpreter& it, const std::string& name) {
    const auto v = it.variables().get(name);
    return v && v->kind == ValueKind::Int ? v->int_val : -9999;
}
std::optional<std::string> var_str(Interpreter& it, const std::string& name) {
    const auto v = it.variables().get(name);
    if (!v || v->kind != ValueKind::String) return std::nullopt;
    return v->str_val;
}

// V1: 装上固定历史钩子，验证三个 backlog var system 的落值。
void test_canned_hooks() {
    Interpreter it;
    it.hooks().backlog_size = []() -> size_t { return 3; };
    it.hooks().backlog_tags = [](size_t page, bool allfont)
        -> std::optional<std::vector<std::string>> {
        if (page >= 3) return std::nullopt;
        std::vector<std::string> tags;
        if (allfont) tags.push_back("[font size=\"40\"]");
        tags.push_back("[print data=\"page" + std::to_string(page) + "\"]");
        return tags;
    };
    it.hooks().message_tags = [](const std::string& id, bool)
        -> std::optional<std::vector<std::string>> {
        if (id != "adv01") return std::nullopt;
        return std::vector<std::string>{"[print data=\"cur\"]"};
    };

    it.apply_var(tag({{"system", "get_backlog_size"}, {"name", "t.n"}}));
    check(var_int(it, "t.n") == 3, "V1 get_backlog_size writes the page count");

    it.apply_var(tag(
        {{"system", "get_backlog_tags"}, {"name", "t.bl"}, {"page", "1"}, {"allfont", "1"}}));
    check(var_int(it, "t.bl.size") == 2, "V1 get_backlog_tags pseudo size");
    const auto s0 = var_str(it, "t.bl.0");
    const auto s1 = var_str(it, "t.bl.1");
    check(s0 && *s0 == "[font size=\"40\"]", "V1 allfont prepends the font tag");
    check(s1 && *s1 == "[print data=\"page1\"]", "V1 page tag follows");

    // 越界页 → None → 只落 size=0，不留脏索引。
    it.apply_var(tag({{"system", "get_backlog_tags"}, {"name", "t.o"}, {"page", "9"}}));
    check(var_int(it, "t.o.size") == 0, "V1 out-of-range page writes size=0");
    check(it.variables().get("t.o.0").has_value() == false,
          "V1 out-of-range leaves no index keys");

    it.apply_var(tag({{"system", "get_message_tags"}, {"name", "t.m"}, {"id", "adv01"}}));
    check(var_int(it, "t.m.size") == 1, "V1 get_message_tags pseudo size");
    const auto m0 = var_str(it, "t.m.0");
    check(m0 && *m0 == "[print data=\"cur\"]", "V1 message tag follows");
    // 层不存在 → None → size=0。
    it.apply_var(tag({{"system", "get_message_tags"}, {"name", "t.x"}, {"id", "nope"}}));
    check(var_int(it, "t.x.size") == 0, "V1 unknown layer writes size=0");
}

// V2: 无钩子 → 保守回退（size=0 / 空伪数组），与 with_host_hooks 回退一致。
void test_no_hooks_fallback() {
    Interpreter it; // 未安装任何 backlog 钩子
    it.apply_var(tag({{"system", "get_backlog_size"}, {"name", "t.n"}}));
    check(var_int(it, "t.n") == 0, "V2 no hook backlog_size falls back to 0");
    it.apply_var(tag({{"system", "get_backlog_tags"}, {"name", "t.bl"}, {"page", "0"}}));
    check(var_int(it, "t.bl.size") == 0, "V2 no hook backlog_tags size=0");
    it.apply_var(tag({{"system", "get_message_tags"}, {"name", "t.m"}, {"id", "adv01"}}));
    check(var_int(it, "t.m.size") == 0, "V2 no hook message_tags size=0");
}

// V4: 钩子接到真实 TextEngine（runtime.cpp 的接线形状）→ 读到 P3b 数据面。
void test_engine_integration() {
    TextEngine e;
    e.set_backlog_write_mode(true);
    e.font_default(tag({{"size", "40"}}));
    e.push_text("こんにちは");
    e.apply_font(tag({{"color", "FF0000"}}));
    e.line_break(true);
    e.push_text("次");
    check(e.backlog_size() == 0, "V4 nothing stored before rp");
    e.page_break(std::nullopt); // write_mode=1 → 入库 1 页
    check(e.backlog_size() == 1, "V4 rp archived one page");

    // 接线形状同 runtime.cpp open_project 的 hooks 安装。
    Interpreter it;
    it.hooks().backlog_size = [&e]() -> size_t { return e.backlog_size(); };
    it.hooks().backlog_tags = [&e](size_t page, bool allfont)
        -> std::optional<std::vector<std::string>> { return e.backlog_tags(page, allfont); };
    it.hooks().message_tags = [&e](const std::string& id, bool allfont)
        -> std::optional<std::vector<std::string>> { return e.message_tags(id, allfont); };

    it.apply_var(tag({{"system", "get_backlog_size"}, {"name", "t.n"}}));
    check(var_int(it, "t.n") == 1, "V4 backlog_size reads the engine count");

    // 换页后活动层页标签已清空 → 页 0 的归档序列仍可读（含 allfont 前缀）。
    it.apply_var(tag(
        {{"system", "get_backlog_tags"}, {"name", "t.bl"}, {"page", "0"}, {"allfont", "1"}}));
    check(var_int(it, "t.bl.size") == 5, "V4 archived page size 5");
    const auto b0 = var_str(it, "t.bl.0");
    check(b0 && b0->rfind("[font ", 0) == 0 && b0->find("size=\"40\"") != std::string::npos,
          "V4 allfont prepends page-start font");
    const auto b1 = var_str(it, "t.bl.1");
    check(b1 && *b1 == "[print data=\"こんにちは\"]", "V4 first print tag");
    const auto b2 = var_str(it, "t.bl.2");
    check(b2 && b2->find("FF0000") != std::string::npos, "V4 font tag kept");
    const auto b3 = var_str(it, "t.bl.3");
    check(b3 && *b3 == "[rt]", "V4 rt tag kept");
    const auto b4 = var_str(it, "t.bl.4");
    check(b4 && *b4 == "[print data=\"次\"]", "V4 second print tag");

    it.apply_var(tag({{"system", "get_backlog_tags"}, {"name", "t.o"}, {"page", "5"}}));
    check(var_int(it, "t.o.size") == 0, "V4 engine out-of-range page size=0");

    // 当前页 message_tags：换页后的新页（空）→ size=0；往活动层再写内容可读。
    it.apply_var(tag({{"system", "get_message_tags"}, {"name", "t.m"}, {"id", "adv01"}}));
    check(var_int(it, "t.m.size") == 0, "V4 message tags cleared after rp");
    e.push_text("次ページ");
    it.apply_var(tag({{"system", "get_message_tags"}, {"name", "t.m"}, {"id", "adv01"}}));
    check(var_int(it, "t.m.size") == 1, "V4 new-page message tag size");
    const auto m0 = var_str(it, "t.m.0");
    check(m0 && *m0 == "[print data=\"次ページ\"]", "V4 message tag content");
    it.apply_var(tag({{"system", "get_message_tags"}, {"name", "t.u"}, {"id", "nope"}}));
    check(var_int(it, "t.u.size") == 0, "V4 unknown layer size=0");
}
} // namespace

int main() {
    test_canned_hooks();
    test_no_hooks_fallback();
    test_engine_integration();
    if (failures > 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("backlog_var_bridge: ok\n");
    return 0;
}
