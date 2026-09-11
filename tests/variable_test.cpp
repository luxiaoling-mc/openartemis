// Value/VariableStore/ExpressionEvaluator tests (M3b).
#include <cstdio>
#include <string>

#include "core/runtime/runtime_iet.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using oa::runtime::Value;
using oa::runtime::ValueKind;
using oa::runtime::VariableStore;
using oa::runtime::ExpressionEvaluator;
using oa::runtime::ExpressionError;

struct Ev {
    VariableStore vars;
    ExpressionEvaluator eval{vars};
    Value run(const std::string& expr) {
        return eval.evaluate(oa::runtime::normalize_expr(expr));
    }
};

void test_value_conversions() {
    check(Value::make_bool(true).as_int() == 1, "bool->int");
    check(Value::make_string("0").to_bool(), "string '0' truthy");
    check(!Value::make_string("").to_bool(), "empty string falsy");
    check(Value::make_null().as_int() == 0 && Value::make_null().as_float() == 0.0,
          "null numeric zero");
    check(!Value::make_null().to_bool(), "null falsy");
    check(Value::make_float(1.8).to_string() == "1.8", "float to_string 1.8");
    check(Value::make_float(2.0).to_string() == "2", "float to_string 2.0");
    check(Value::make_int(5).to_string() == "5", "int to_string");
    check(Value::make_string("42").as_int() == 42, "string->int");
    check(!Value::make_string("1.5").as_int().has_value(), "float-string no int");
    check(Value::make_string("1.5").as_float() == 1.5, "string->float");
    check(!Value::make_string("1.5x").as_float().has_value(), "bad string no float");
    check(Value::make_float(1.8).to_display() == "1.8", "float display");
    check(Value::make_bool(false).to_string() == "0", "bool to_string");
}

void test_store_domains() {
    VariableStore v;
    v.set("foo", Value::make_int(1));
    v.set("g.score", Value::make_int(7));
    v.set("t.tmp", Value::make_string("x"));
    v.set("s.savepath", Value::make_string("save"));
    check(v.get("foo")->as_int() == 1, "local");
    check(v.get("g.score")->as_int() == 7, "global");
    check(v.get("t.tmp")->to_string() == "x", "temp");
    check(v.get("s.savepath")->to_string() == "save", "system");
    check(!v.get("g.foo").has_value(), "missing");
    v.reset_local_temp();
    check(!v.get("foo").has_value() && !v.get("t.tmp").has_value(), "reset clears local+temp");
    check(v.get("g.score").has_value() && v.get("s.savepath").has_value(),
          "reset keeps g/s");
}

void test_arith() {
    Ev e;
    check(e.run("2 + 3 * 4") == Value::make_int(14), "precedence");
    check(e.run("(2 + 3) * 4") == Value::make_int(20), "parens");
    check(e.run("10 / 4") == Value::make_int(2), "int division truncates");
    check(e.run("10 % 3") == Value::make_int(1), "mod");
    check(e.run("7 - 10") == Value::make_int(-3), "neg result");
    check(e.run("-5 + 2") == Value::make_int(-3), "unary minus");
    check(e.run("1.5 + 1") == Value::make_float(2.5), "mixed float");
    check(e.run("1 + 1.5") == Value::make_float(2.5), "mixed float rev");
    check(e.run("'a' + 'b'") == Value::make_string("ab"), "string concat");
    check(e.run("'n=' + 5") == Value::make_string("n=5"), "string+int concat");
    check(e.run("5 + 'x'") == Value::make_string("5x"), "int+string concat");
    check(e.run("'1.5' + 1") == Value::make_string("1.51"), "numeric-string is string");
    check(e.run("0x10") == Value::make_int(16), "hex literal");
    check(e.run("0xFF - 1") == Value::make_int(254), "hex arith");
    bool threw = false;
    try {
        (void)e.run("1 / 0");
    } catch (const ExpressionError&) {
        threw = true;
    }
    check(threw, "div by zero raises");
    threw = false;
    try {
        (void)e.run("1 % 0");
    } catch (const ExpressionError&) {
        threw = true;
    }
    check(threw, "mod by zero raises");
}

void test_compare_logic() {
    Ev e;
    check(e.run("2 == 2") == Value::make_bool(true), "eq int");
    check(e.run("2 != 3") == Value::make_bool(true), "neq");
    check(e.run("1 < 2 && 2 <= 2") == Value::make_bool(true), "rel + and");
    check(e.run("3 > 4 || 4 >= 4") == Value::make_bool(true), "or");
    check(e.run("'1' == 1") == Value::make_bool(true), "numeric string eq via f64");
    check(e.run("'abc' == 'xyz'") == Value::make_bool(true),
          "non-numeric strings compare as 0==0 (Artemis semantics)");
    check(e.run("'abc' == 0") == Value::make_bool(true), "string vs 0 via f64 fallback");
    check(e.run("2.5 < 3") == Value::make_bool(true), "float rel");
}

void test_variables_and_missing() {
    Ev e;
    e.vars.set("t.check", Value::make_int(0));
    e.vars.set("g.score", Value::make_int(7));
    check(e.run("$t.check == 0") == Value::make_bool(true), "var eq");
    check(e.run("$g.score * 2") == Value::make_int(14), "var arith");
    check(e.run("$missing == 0") == Value::make_bool(true), "missing var == 0");
    check(e.run("$missing + 5") == Value::make_int(5), "missing var arith");
    e.vars.set("t.name", Value::make_string("Alice"));
    check(e.run("$t.name") == Value::make_string("Alice"), "string var value");
    // dynamic name foo.(expr)
    e.vars.set("t.pos.1", Value::make_int(3));
    e.vars.set("t.pos", Value::make_int(1));
    check(e.run("$t.pos.(1)") == Value::make_int(3), "dynamic name");
    // missing id with parens chain fallback 0
    check(e.run("$nope.(1)") == Value::make_int(0), "dynamic missing = 0");
}

void test_strip_dollar() {
    check(oa::runtime::normalize_expr("$t.x == 'cost $5'") == "t.x == 'cost $5'",
          "strip keeps literal $");
    check(oa::runtime::normalize_expr("$a+$b") == "a+b", "strip inner refs");
}

void test_resolve_param() {
    VariableStore v;
    v.set("t.x", Value::make_int(1));
    ExpressionEvaluator ev(v);
    check(ev.resolve_param("$t.x + 1") == Value::make_int(2), "resolve expr");
    check(ev.resolve_param("'hello'") == Value::make_string("hello"), "resolve literal");
    check(ev.resolve_param("42") == Value::make_int(42), "resolve int");
    check(ev.resolve_param("4.5") == Value::make_float(4.5), "resolve float");
    check(ev.resolve_param("hello") == Value::make_string("hello"), "resolve string");
    check(ev.resolve_param_str("1.80") == "1.80", "resolve_param_str keeps 1.80");
    check(ev.resolve_param_str("$t.x") == "1", "resolve_param_str var");
    check(ev.resolve_param_str("'1.80'") == "1.80", "resolve_param_str literal");
}

void test_string_compare_cases() {
    Ev e;
    e.vars.set("s.sp", Value::make_int(0));
    check(e.run("$s.sp==0") == Value::make_bool(true), "cond s.sp==0");
    e.vars.set("t.file", Value::make_string("x.dat"));
    check(e.run("$t.file=='x.dat'") == Value::make_bool(true),
          "string comparison via f64 (both 0)");
}

} // namespace

int main() {
    test_value_conversions();
    test_store_domains();
    test_arith();
    test_compare_logic();
    test_variables_and_missing();
    test_strip_dollar();
    test_resolve_param();
    test_string_compare_cases();
    if (failures) {
        std::fprintf(stderr, "variable_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("variable_test: all ok\n");
    return 0;
}
