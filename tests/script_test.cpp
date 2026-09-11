// Script text parser tests.
#include <cstdio>
#include <string>
#include <vector>

#include "core/runtime/runtime_iet.h"
#include "core/fs/physfs_fs.h"
namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

template <typename A, typename B>
void check_eq(const A& a, const B& b, const char* what) {
    if (!(a == b)) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// Row-stream labels for ordering assertions: real tags keep their name;
// synthesized rows show their kind (they carry no tag name by design).
std::string row_label(const oa::runtime::Instruction& i) {
    switch (i.kind) {
        case oa::runtime::Instruction::Kind::Text:
            return "TEXT";
        case oa::runtime::Instruction::Kind::LuaBlock:
            return "LUA_BLOCK";
        default:
            return i.tag;
    }
}

std::vector<std::string> tags_of(const oa::runtime::Script& s) {
    std::vector<std::string> out;
    for (const auto& i : s.instructions) out.push_back(row_label(i));
    return out;
}

void test_parse_simple_script() {
    const std::string content = R"(
*main
[calllua function="scriptMainloop"]
[jump label="next"]
*next
[return]
)";
    const auto s = oa::runtime::Script::parse("test", content);
    check(s.labels.at("main") == 0, "label main at 0");
    check(s.labels.at("next") == 2, "label next at 2");
    check_eq(s.instructions.size(), size_t(3), "three instructions");
    check_eq(s.instructions[0].tag, std::string("calllua"), "instr0 calllua");
    const std::string* f = s.instructions[0].get("function");
    check(f && *f == "scriptMainloop", "function param");
    check_eq(s.instructions[1].get_or("label", ""), std::string("next"), "jump label");
}

void test_parse_text() {
    const std::string content = "*main\n这是剧情文本\n[return]\n";
    const auto s = oa::runtime::Script::parse("test", content);
    check(s.instructions[0].kind == oa::runtime::Instruction::Kind::Text,
          "story text is a Text row (no reserved tag name)");
    check(s.instructions[0].tag.empty(), "text row carries no tag name");
    check_eq(s.instructions[0].get_or("text", ""), std::string("这是剧情文本"),
             "story text value");
    check(s.instructions[0].line == 2, "text line number");
}

void test_empty_label_means_file_start() {
    const std::string content = "[stop]\n*main\n[jump]\n";
    const auto s = oa::runtime::Script::parse("test", content);
    check(s.get_label_line("") == 0, "empty label -> 0");
    check(s.get_label_line("main") == 1, "main -> 1");
    check(!s.get_label_line("missing").has_value(), "missing label");
}

void test_mixed_text_and_tags_in_one_line() {
    const auto s = oa::runtime::Script::parse("test", "[onlinehead]「ぷるぷる」@[rp]");
    check_eq(tags_of(s), (std::vector<std::string>{"onlinehead", "TEXT", "rp"}),
             "mixed segments order");
    check_eq(s.instructions[1].get_or("text", ""), std::string("「ぷるぷる」@"),
             "inline text preserved");
}

void test_trailing_line_comment_after_tag_is_dropped() {
    const auto s = oa::runtime::Script::parse("test", "[rt] // 注释 [rp]");
    check_eq(tags_of(s), (std::vector<std::string>{"rt"}), "inline comment dropped");
}

void test_block_comment_cross_line() {
    // The real-world shape: a whole "/*" block with active-looking tags,
    // label lines and trailing "//" comments inside, closed by "*/".
    const std::string content = R"(*main
[wait 100]
/*
 * fake label line
[var name="t.x" data="1"]	// trailing comment inside the block
[if estimate="t.x == 1"]
    [exit]
[/if]
*/
*after
[wait 200]
)";
    const auto s = oa::runtime::Script::parse("test", content);
    check_eq(s.instructions.size(), size_t(2), "two wait instructions");
    check_eq(tags_of(s), (std::vector<std::string>{"wait", "wait"}), "block content inert");
    check(s.labels.count("main") == 1 && s.labels.count("after") == 1,
          "labels outside block");
    check(s.labels.count("fake label line") == 0, "block content not a label");
    check(s.instructions[0].line == 2 && s.instructions[1].line == 11,
          "line numbers skip the block");
}

void test_block_comment_one_line() {
    // Comment before and after an instruction on one line, incl. brackets
    // and line comments inside the block (opaque content).
    const auto s1 = oa::runtime::Script::parse("test", "/* note */ [wait 10]");
    check_eq(tags_of(s1), (std::vector<std::string>{"wait"}), "block before tag");
    const auto s2 = oa::runtime::Script::parse("test", "[wait 10] /* note */");
    check_eq(tags_of(s2), (std::vector<std::string>{"wait"}), "block after tag");
    const auto s3 = oa::runtime::Script::parse("test", "[a] /* [b] // x */ [c]");
    check_eq(tags_of(s3), (std::vector<std::string>{"a", "c"}),
             "brackets/line comments inside block are opaque");
    const auto s4 = oa::runtime::Script::parse("test", "/* one */ [wait 10] /* two */ [stop]");
    check_eq(tags_of(s4), (std::vector<std::string>{"wait", "stop"}),
             "several inline blocks");
}

void test_block_comment_closer_with_code() {
    // A block closing mid-line may leave code on the closer's line.
    const auto s = oa::runtime::Script::parse("test", "[wait 10]\n/* multi\nline */ [wait 20]");
    check_eq(tags_of(s), (std::vector<std::string>{"wait", "wait"}), "code after closer");
    check_eq(s.instructions[1].get_or("0", ""), std::string("20"), "param kept");
    check(s.instructions[1].line == 3, "closer line attributed");
}

void test_block_comment_markers_in_params_and_text_are_literal() {
    // Inside "[...]" parameters (incl. quoted values) '/' never starts a
    // comment; markers mid story text are literal too (same as "//").
    const auto s = oa::runtime::Script::parse(
        "test", "[var name=\"a/*b\" data=\"x//y\"] /* real */\nabc /* x */ def\n");
    check_eq(tags_of(s), (std::vector<std::string>{"var", "TEXT"}), "one tag + text");
    check_eq(s.instructions[0].get_or("name", ""), std::string("a/*b"),
             "slash-star inside quote kept");
    check_eq(s.instructions[0].get_or("data", ""), std::string("x//y"),
             "slash-slash inside quote kept");
    check_eq(s.instructions[1].get_or("text", ""), std::string("abc /* x */ def"),
             "mid-text markers literal");
}

void test_block_comment_mixing_line_comments() {
    const auto s = oa::runtime::Script::parse(
        "test",
        "// line comment\n; semicolon comment\n// /* not a block\n"
        "[wait 10] // /* still not a block\n"
        "[wait 20] /* block */ // after\n[stop]");
    check_eq(tags_of(s), (std::vector<std::string>{"wait", "wait", "stop"}),
             "comments mix cleanly");
    check(s.instructions[1].line == 5, "line 5 instruction kept");
}

void test_block_comment_unterminated_throws() {
    bool threw = false;
    try {
        (void)oa::runtime::Script::parse("test", "[stop]\n/* never closed\n[exit]\n");
    } catch (const oa::runtime::ParseError& e) {
        threw = true;
        check(e.line == 2, "unterminated block reported at opener");
        check(std::string(e.what()).find("unterminated block comment") != std::string::npos,
              "unterminated block message");
    }
    check(threw, "unterminated block comment throws");
}

void test_lua_block() {
    const std::string content = "[lua]\nlocal a = 1\n\nreturn a\n[/lua]\n[stop]";
    const auto s = oa::runtime::Script::parse("test", content);
    check_eq(tags_of(s), (std::vector<std::string>{"LUA_BLOCK", "stop"}),
             "lua block + stop");
    check(s.instructions[0].kind == oa::runtime::Instruction::Kind::LuaBlock &&
              s.instructions[0].tag.empty(),
          "lua block is a LuaBlock row (no reserved tag name)");
    check_eq(s.instructions[0].get_or("code", ""), std::string("local a = 1\n\nreturn a"),
             "lua block content preserved");
    check_eq(s.instructions[1].get_or("x", "d"), std::string("d"), "stop no params");
}

void test_instruction_methods() {
    oa::runtime::Instruction ins;
    ins.tag = "test";
    ins.params["key1"] = "value1";
    ins.params["0"] = "default";
    ins.line = 1;
    check_eq(ins.get_or("key1", ""), std::string("value1"), "get key1");
    check(!ins.get("key2"), "get missing -> null");
    check_eq(ins.get_or("key2", "fallback"), std::string("fallback"), "fallback");
    check(ins.has("key1") && !ins.has("key2"), "has");
    const std::string* d = ins.get_default();
    check(d && *d == "default", "get_default prefers 0");
    oa::runtime::Instruction named;
    named.tag = "t";
    named.params["alpha"] = "1";
    const std::string* d2 = named.get_default();
    check(d2 && *d2 == "1", "get_default falls back to first key");
}

void test_unclosed_quote_errors() {
    bool threw = false;
    try {
        (void)oa::runtime::parse_params("a=\"x b", 1);
    } catch (const oa::runtime::ParseError& e) {
        threw = true;
        check(std::string(e.what()).find("unterminated quote") != std::string::npos,
              "unclosed quote message");
    }
    check(threw, "unclosed quote throws");
    bool threw2 = false;
    try {
        (void)oa::runtime::Script::parse("test", "[lua]\nx=1\n");
    } catch (const oa::runtime::ParseError&) {
        threw2 = true;
    }
    check(threw2, "missing /lua throws");
}

void test_unclosed_bracket_kept_as_text() {
    // "[..." tail is split: story text before '[', then text with '[' restored.
    const auto s = oa::runtime::Script::parse("test", "半角の[カッコ");
    check_eq(tags_of(s), (std::vector<std::string>{"TEXT", "TEXT"}),
             "two text segments");
    check_eq(s.instructions[0].get_or("text", ""), std::string("半角の"),
             "text before bracket");
    check_eq(s.instructions[1].get_or("text", ""), std::string("[カッコ"),
             "bracket restored in tail text");
}

} // namespace

void test_real_first_iet() {
    const char* pfs_path = std::getenv("OA_TEST_FPM_PFS");
    if (!pfs_path || !*pfs_path) return;
        oa::fs::PhysFileSystem fs(pfs_path, false);
    auto bytes = fs.read("system/first.iet");
    check(bytes.has_value(), "first.iet readable");
    if (!bytes) return;
    const std::string text(bytes->begin(), bytes->end());
    const auto s = oa::runtime::Script::parse("system/first.iet", text);
    check(s.instructions.size() > 20, "first.iet parsed to >20 instructions");
    for (const char* lbl : {"top", "game_start", "title", "movie_emergendcy", "last"}) {
        check(s.labels.count(lbl) == 1, "first.iet label present");
    }
    // boot: calllua system_initlua then [wt], [stop] at *top end
    check(s.instructions[0].kind == oa::runtime::Instruction::Kind::LuaBlock,
          "first instruction lua block");
    bool saw_init = false, saw_loading = false;
    for (const auto& i : s.instructions) {
        const std::string* f = i.get("function");
        if (f && *f == "system_initlua") saw_init = true;
        if (f && *f == "system_loadinglua") saw_loading = true;
    }
    check(saw_init && saw_loading, "calllua functions present");
    // lua blocks exist (inline functions defined in the iet)
    bool has_lua = false;
    for (const auto& i : s.instructions)
        if (i.kind == oa::runtime::Instruction::Kind::LuaBlock) has_lua = true;
    check(has_lua, "first.iet has [lua] blocks");
    std::printf("first.iet: %zu instructions, %zu labels\n", s.instructions.size(),
                s.labels.size());
}

int main() {
    test_parse_simple_script();
    test_real_first_iet();
    test_parse_text();
    test_empty_label_means_file_start();
    test_mixed_text_and_tags_in_one_line();
    test_trailing_line_comment_after_tag_is_dropped();
    test_block_comment_cross_line();
    test_block_comment_one_line();
    test_block_comment_closer_with_code();
    test_block_comment_markers_in_params_and_text_are_literal();
    test_block_comment_mixing_line_comments();
    test_block_comment_unterminated_throws();
    test_lua_block();
    test_instruction_methods();
    test_unclosed_quote_errors();
    test_unclosed_bracket_kept_as_text();
    if (failures) {
        std::fprintf(stderr, "script_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("script_test: all ok\n");
    return 0;
}
