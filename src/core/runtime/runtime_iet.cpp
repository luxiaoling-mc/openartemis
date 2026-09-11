// ===========================================================================
// runtime_iet.cpp — the complete oa::runtime module in ONE translation unit.
//
// The module runs Artemis scenario data: the value/variable model, the
// expression evaluator, the .iet/.ast text parser, the .asb binary decoder
// and the interpreter that executes instructions (control tags, waits, Lua
// hooks, script store + call stack + save/restore of the execution position).
//
// Organization follows krkrsdl3 cpp/core/script/tjsNativeKAGParser.cpp, the
// KAG scenario unit this format descends from: scenario cache/loading,
// parsing, execution state and the save/restore of that state live in one
// file, sectioned top-down —
//    values/expressions -> instruction + script parsing (.iet/.asb) ->
//    interpreter/execution -> hooks.
// Only the organization is borrowed; no KAG code is.
//
// Interface: runtime/runtime_iet.h is the single (thin) public header. Every
// helper here — parsers, tag dispatch, depth scans, formatting, the Lua host
// bootstrap — is internal linkage (anonymous namespace) and invisible to
// callers. Keep it that way: "small interface, big file".
// ===========================================================================
#include "core/runtime/runtime_iet.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/media/image.h"
#include "core/util/charset.h"

namespace oa::runtime {

namespace {
bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}
} // namespace

// ---------------------------------------------------------------------------
// §1 values — Value conversions + VariableStore domains
// ---------------------------------------------------------------------------

std::optional<int64_t> Value::as_int() const {
    switch (kind) {
        case ValueKind::Int:
            return int_val;
        case ValueKind::Float:
            return int64_t(float_val);
        case ValueKind::Bool:
            return bool_val ? 1 : 0;
        case ValueKind::Null:
            return 0;
        case ValueKind::String: {
            // Rust parse::<i64> semantics: full-string, optional sign.
            const char* p = str_val.c_str();
            char* end = nullptr;
            errno = 0;
            const long long v = std::strtoll(p, &end, 10);
            if (errno != 0 || end == p || *end != '\0') return std::nullopt;
            return int64_t(v);
        }
    }
    return std::nullopt;
}

std::optional<double> Value::as_float() const {
    switch (kind) {
        case ValueKind::Int:
            return double(int_val);
        case ValueKind::Float:
            return float_val;
        case ValueKind::Bool:
            return bool_val ? 1.0 : 0.0;
        case ValueKind::Null:
            return 0.0;
        case ValueKind::String: {
            const char* p = str_val.c_str();
            char* end = nullptr;
            errno = 0;
            const double v = std::strtod(p, &end);
            if (errno != 0 || end == p || *end != '\0') return std::nullopt;
            return v;
        }
    }
    return std::nullopt;
}

bool Value::to_bool() const {
    switch (kind) {
        case ValueKind::Int:
            return int_val != 0;
        case ValueKind::Float:
            return float_val != 0.0;
        case ValueKind::Bool:
            return bool_val;
        case ValueKind::String:
            return !str_val.empty(); // non-empty string is truthy, even "0"
        case ValueKind::Null:
            return false;
    }
    return false;
}

std::string Value::to_string() const {
    switch (kind) {
        case ValueKind::Int:
            return std::to_string(int_val);
        case ValueKind::Float:
            return to_display();
        case ValueKind::Bool:
            return bool_val ? "1" : "0";
        case ValueKind::String:
            return str_val;
        case ValueKind::Null:
            return "";
    }
    return "";
}

std::string Value::to_display() const {
    switch (kind) {
        case ValueKind::Float: {
            // Shortest round-trip repr without exponent for typical ranges
            // (float display formatting; engine-internal consistent).
            char buf[64];
            auto res = std::to_chars(buf, buf + sizeof(buf), float_val);
            if (res.ec == std::errc()) {
                std::string s(buf, res.ptr);
                // Rust never prints "+"; to_chars prints integers w/o trailing
                // ".0" already under default (shortest) format.
                return s;
            }
            return std::to_string(float_val);
        }
        default:
            return to_string();
    }
}

VariableStore::Map* VariableStore::domain_of(const std::string& name) {
    if (name.rfind("g.", 0) == 0) return &global;
    if (name.rfind("s.", 0) == 0) return &system;
    if (name.rfind("t.", 0) == 0) return &temp;
    return &local;
}

const VariableStore::Map* VariableStore::domain_of(const std::string& name) const {
    if (name.rfind("g.", 0) == 0) return &global;
    if (name.rfind("s.", 0) == 0) return &system;
    if (name.rfind("t.", 0) == 0) return &temp;
    return &local;
}

std::optional<Value> VariableStore::get(const std::string& name) const {
    const Map* m = domain_of(name);
    const auto it = m->find(name);
    if (it == m->end()) return std::nullopt;
    return it->second;
}

void VariableStore::set(const std::string& name, Value value) {
    (*domain_of(name))[name] = std::move(value);
}

void VariableStore::remove(const std::string& name) { domain_of(name)->erase(name); }

void VariableStore::reset_local_temp() {
    local.clear();
    temp.clear();
}

void VariableStore::clear_all() {
    local.clear();
    global.clear();
    temp.clear();
    system.clear();
}

// ---------------------------------------------------------------------------
// §2 expressions — tokenizer + recursive-descent evaluator
// ---------------------------------------------------------------------------

namespace {

bool is_ident_char(char c) {
    return std::isalnum(uint8_t(c)) || c == '_' || c == '.';
}

} // namespace

std::string normalize_expr(std::string_view expr) {
    std::string result;
    result.reserve(expr.size());
    size_t i = 0;
    const size_t n = expr.size();
    while (i < n) {
        const char c = expr[i];
        if (c == '\'') {
            // string literal: keep as-is
            result.push_back(c);
            ++i;
            while (i < n) {
                const char cc = expr[i++];
                result.push_back(cc);
                if (cc == '\'') break;
            }
        } else if (c == '$') {
            if (i + 1 < n) {
                const char nx = expr[i + 1];
                if (std::isalpha(uint8_t(nx)) || nx == '_' || nx == '.') {
                    ++i; // skip '$'
                    continue;
                }
            }
            result.push_back('$');
            ++i;
        } else {
            result.push_back(c);
            ++i;
        }
    }
    return result;
}

namespace {

enum class Tok {
    Number, // raw text
    String, // decoded text
    Ident,  // raw text
    Plus, Minus, Star, Slash, Percent, Eq, Neq, Lt, Le, Gt, Ge, And, Or,
    LParen, RParen, Comma, End,
};

struct Token {
    Tok kind;
    std::string text;
};

std::vector<Token> tokenize(std::string_view input) {
    std::vector<Token> out;
    size_t i = 0;
    const size_t n = input.size();
    auto fail = [&](const std::string& m) -> std::vector<Token> {
        throw ExpressionError(m);
    };
    while (i < n) {
        const char c = input[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++i;
            continue;
        }
        switch (c) {
            case '+': out.push_back({Tok::Plus, {}}); ++i; continue;
            case '-': out.push_back({Tok::Minus, {}}); ++i; continue;
            case '*': out.push_back({Tok::Star, {}}); ++i; continue;
            case '/': out.push_back({Tok::Slash, {}}); ++i; continue;
            case '%': out.push_back({Tok::Percent, {}}); ++i; continue;
            case '(': out.push_back({Tok::LParen, {}}); ++i; continue;
            case ')': out.push_back({Tok::RParen, {}}); ++i; continue;
            case ',': out.push_back({Tok::Comma, {}}); ++i; continue;
            case '=':
                if (i + 1 < n && input[i + 1] == '=') {
                    out.push_back({Tok::Eq, {}});
                    i += 2;
                } else {
                    return fail("expected ==");
                }
                continue;
            case '!':
                if (i + 1 < n && input[i + 1] == '=') {
                    out.push_back({Tok::Neq, {}});
                    i += 2;
                } else {
                    return fail("unexpected '!'");
                }
                continue;
            case '<':
                if (i + 1 < n && input[i + 1] == '=') {
                    out.push_back({Tok::Le, {}});
                    i += 2;
                } else {
                    out.push_back({Tok::Lt, {}});
                    ++i;
                }
                continue;
            case '>':
                if (i + 1 < n && input[i + 1] == '=') {
                    out.push_back({Tok::Ge, {}});
                    i += 2;
                } else {
                    out.push_back({Tok::Gt, {}});
                    ++i;
                }
                continue;
            case '&':
                if (i + 1 < n && input[i + 1] == '&') {
                    out.push_back({Tok::And, {}});
                    i += 2;
                } else {
                    return fail("expected &&");
                }
                continue;
            case '|':
                if (i + 1 < n && input[i + 1] == '|') {
                    out.push_back({Tok::Or, {}});
                    i += 2;
                } else {
                    return fail("expected ||");
                }
                continue;
            case '\'': {
                ++i;
                std::string s;
                bool closed = false;
                while (i < n) {
                    const char cc = input[i++];
                    if (cc == '\'') {
                        closed = true;
                        break;
                    }
                    s.push_back(cc);
                }
                if (!closed) return fail("unterminated string literal");
                out.push_back({Tok::String, std::move(s)});
                continue;
            }
            default: break;
        }
        if (std::isdigit(uint8_t(c)) || c == '.') {
            // Hex: only "0x" form consumes hex digits.
            if (c == '0' && i + 1 < n && (input[i + 1] == 'x' || input[i + 1] == 'X')) {
                size_t b = i;
                i += 2;
                while (i < n && std::isxdigit(uint8_t(input[i]))) ++i;
                out.push_back({Tok::Number, std::string(input.substr(b, i - b))});
                continue;
            }
            size_t b = i;
            while (i < n && (std::isdigit(uint8_t(input[i])) || input[i] == '.')) ++i;
            out.push_back({Tok::Number, std::string(input.substr(b, i - b))});
            continue;
        }
        if (std::isalpha(uint8_t(c)) || c == '_' || c == '.') {
            size_t b = i;
            while (i < n && is_ident_char(input[i])) ++i;
            out.push_back({Tok::Ident, std::string(input.substr(b, i - b))});
            continue;
        }
        return fail("unexpected character in expression");
    }
    out.push_back({Tok::End, {}});
    return out;
}

class Parser {
public:
    Parser(std::vector<Token> tokens, const VariableStore& vars)
        : toks_(std::move(tokens)), vars_(&vars) {}

    Value parse() { return parse_or(); }

private:
    const Token& peek() const {
        static const Token kEnd{Tok::End, ""};
        return pos_ < toks_.size() ? toks_[pos_] : kEnd;
    }
    Token next() {
        const Token t = peek();
        if (pos_ < toks_.size()) ++pos_;
        return t;
    }
    void expect(Tok k) {
        if (peek().kind != k) throw ExpressionError("expected token");
        ++pos_;
    }
    static bool tok_eq(const Token& t, Tok k) { return t.kind == k; }

    Value parse_or() {
        Value left = parse_and();
        while (tok_eq(peek(), Tok::Or)) {
            next();
            const Value right = parse_and();
            left = Value::make_bool(left.to_bool() || right.to_bool());
        }
        return left;
    }
    Value parse_and() {
        Value left = parse_equality();
        while (tok_eq(peek(), Tok::And)) {
            next();
            const Value right = parse_equality();
            left = Value::make_bool(left.to_bool() && right.to_bool());
        }
        return left;
    }
    Value parse_equality() {
        Value left = parse_comparison();
        for (;;) {
            if (tok_eq(peek(), Tok::Eq)) {
                next();
                const Value right = parse_comparison();
                left = compare(left, right, [](double a, double b) { return a == b; });
            } else if (tok_eq(peek(), Tok::Neq)) {
                next();
                const Value right = parse_comparison();
                left = compare(left, right, [](double a, double b) { return a != b; });
            } else {
                break;
            }
        }
        return left;
    }
    Value parse_comparison() {
        Value left = parse_addition();
        for (;;) {
            if (tok_eq(peek(), Tok::Lt)) {
                next();
                const Value right = parse_addition();
                left = compare(left, right, [](double a, double b) { return a < b; });
            } else if (tok_eq(peek(), Tok::Le)) {
                next();
                const Value right = parse_addition();
                left = compare(left, right, [](double a, double b) { return a <= b; });
            } else if (tok_eq(peek(), Tok::Gt)) {
                next();
                const Value right = parse_addition();
                left = compare(left, right, [](double a, double b) { return a > b; });
            } else if (tok_eq(peek(), Tok::Ge)) {
                next();
                const Value right = parse_addition();
                left = compare(left, right, [](double a, double b) { return a >= b; });
            } else {
                break;
            }
        }
        return left;
    }
    Value parse_addition() {
        Value left = parse_multiplication();
        for (;;) {
            if (tok_eq(peek(), Tok::Plus)) {
                next();
                const Value right = parse_multiplication();
                left = add(left, right);
            } else if (tok_eq(peek(), Tok::Minus)) {
                next();
                const Value right = parse_multiplication();
                left = sub(left, right);
            } else {
                break;
            }
        }
        return left;
    }
    Value parse_multiplication() {
        Value left = parse_primary();
        for (;;) {
            if (tok_eq(peek(), Tok::Star)) {
                next();
                const Value right = parse_primary();
                left = mul(left, right);
            } else if (tok_eq(peek(), Tok::Slash)) {
                next();
                const Value right = parse_primary();
                left = div(left, right);
            } else if (tok_eq(peek(), Tok::Percent)) {
                next();
                const Value right = parse_primary();
                left = mod(left, right);
            } else {
                break;
            }
        }
        return left;
    }
    Value parse_primary() {
        const Token t = peek();
        if (t.kind == Tok::Number) {
            next();
            const std::string& n = t.text;
            if (n.size() > 2 && n[0] == '0' && (n[1] == 'x' || n[1] == 'X')) {
                int64_t v = 0;
                const auto r = std::from_chars(n.data() + 2, n.data() + n.size(), v, 16);
                if (r.ec != std::errc() || r.ptr != n.data() + n.size()) {
                    throw ExpressionError("invalid hex number: " + n);
                }
                return Value::make_int(v);
            }
            if (n.find('.') != std::string::npos) {
                char* end = nullptr;
                errno = 0;
                const double v = std::strtod(n.c_str(), &end);
                if (errno != 0 || end != n.c_str() + n.size()) {
                    throw ExpressionError("invalid float: " + n);
                }
                return Value::make_float(v);
            }
            int64_t v = 0;
            const auto r = std::from_chars(n.data(), n.data() + n.size(), v);
            if (r.ec != std::errc() || r.ptr != n.data() + n.size()) {
                throw ExpressionError("invalid integer: " + n);
            }
            return Value::make_int(v);
        }
        if (t.kind == Tok::String) {
            next();
            return Value::make_string(t.text);
        }
        if (t.kind == Tok::Ident) {
            next();
            // dynamic name segments foo.(expr)
            std::string full_name = t.text;
            while (tok_eq(peek(), Tok::LParen)) {
                next();
                const Value inner = parse_expression_inner();
                expect(Tok::RParen);
                full_name += inner.to_string();
                if (tok_eq(peek(), Tok::Ident)) {
                    full_name += next().text;
                }
            }
            if (const auto v = vars_->get(full_name)) return *v;
            return Value::make_int(0); // missing variable == 0
        }
        if (t.kind == Tok::LParen) {
            next();
            const Value v = parse_expression_inner();
            expect(Tok::RParen);
            return v;
        }
        if (t.kind == Tok::Minus) {
            next();
            const Value v = parse_primary();
            if (v.kind == ValueKind::Int) return Value::make_int(-v.int_val);
            if (v.kind == ValueKind::Float) return Value::make_float(-v.float_val);
            throw ExpressionError("unary minus requires a number");
        }
        throw ExpressionError("unexpected token in expression");
    }
    Value parse_expression_inner() { return parse_or(); }

    static Value compare(const Value& a, const Value& b,
                         bool (*fn)(double, double)) {
        const double x = a.as_float().value_or(0.0);
        const double y = b.as_float().value_or(0.0);
        return Value::make_bool(fn(x, y));
    }
    static Value add(const Value& a, const Value& b) {
        if (a.kind == ValueKind::String) {
            return Value::make_string(a.str_val + b.to_display());
        }
        if (b.kind == ValueKind::String) {
            return Value::make_string(a.to_display() + b.str_val);
        }
        if (a.kind == ValueKind::Int && b.kind == ValueKind::Int) {
            return Value::make_int(a.int_val + b.int_val);
        }
        if (a.kind == ValueKind::Float && b.kind == ValueKind::Float) {
            return Value::make_float(a.float_val + b.float_val);
        }
        if (a.kind == ValueKind::Int && b.kind == ValueKind::Float) {
            return Value::make_float(double(a.int_val) + b.float_val);
        }
        if (a.kind == ValueKind::Float && b.kind == ValueKind::Int) {
            return Value::make_float(a.float_val + double(b.int_val));
        }
        return Value::make_int(a.as_int().value_or(0) + b.as_int().value_or(0));
    }
    static Value sub(const Value& a, const Value& b) {
        if (a.kind == ValueKind::Int && b.kind == ValueKind::Int) {
            return Value::make_int(a.int_val - b.int_val);
        }
        if (a.kind == ValueKind::Float && b.kind == ValueKind::Float) {
            return Value::make_float(a.float_val - b.float_val);
        }
        if (a.kind == ValueKind::Int && b.kind == ValueKind::Float) {
            return Value::make_float(double(a.int_val) - b.float_val);
        }
        if (a.kind == ValueKind::Float && b.kind == ValueKind::Int) {
            return Value::make_float(a.float_val - double(b.int_val));
        }
        return Value::make_int(a.as_int().value_or(0) - b.as_int().value_or(0));
    }
    static Value mul(const Value& a, const Value& b) {
        if (a.kind == ValueKind::Int && b.kind == ValueKind::Int) {
            return Value::make_int(a.int_val * b.int_val);
        }
        if (a.kind == ValueKind::Float && b.kind == ValueKind::Float) {
            return Value::make_float(a.float_val * b.float_val);
        }
        if (a.kind == ValueKind::Int && b.kind == ValueKind::Float) {
            return Value::make_float(double(a.int_val) * b.float_val);
        }
        if (a.kind == ValueKind::Float && b.kind == ValueKind::Int) {
            return Value::make_float(a.float_val * double(b.int_val));
        }
        return Value::make_int(a.as_int().value_or(0) * b.as_int().value_or(0));
    }
    static Value div(const Value& a, const Value& b) {
        if (a.kind == ValueKind::Int && b.kind == ValueKind::Int) {
            if (b.int_val == 0) throw ExpressionError("division by zero");
            return Value::make_int(a.int_val / b.int_val);
        }
        if (a.kind == ValueKind::Float && b.kind == ValueKind::Float) {
            if (b.float_val == 0.0) throw ExpressionError("division by zero");
            return Value::make_float(a.float_val / b.float_val);
        }
        if (a.kind == ValueKind::Int && b.kind == ValueKind::Float) {
            if (b.float_val == 0.0) throw ExpressionError("division by zero");
            return Value::make_float(double(a.int_val) / b.float_val);
        }
        if (a.kind == ValueKind::Float && b.kind == ValueKind::Int) {
            if (b.int_val == 0) throw ExpressionError("division by zero");
            return Value::make_float(a.float_val / double(b.int_val));
        }
        const int64_t r = b.as_int().value_or(0);
        if (r == 0) throw ExpressionError("division by zero");
        return Value::make_int(a.as_int().value_or(0) / r);
    }
    static Value mod(const Value& a, const Value& b) {
        const int64_t l = a.as_int().value_or(0);
        const int64_t r = b.as_int().value_or(0);
        if (r == 0) throw ExpressionError("division by zero");
        return Value::make_int(l % r);
    }

    std::vector<Token> toks_;
    const VariableStore* vars_;
    size_t pos_ = 0;
};

} // namespace

Value ExpressionEvaluator::evaluate(std::string_view expr) const {
    Parser parser(tokenize(expr), *vars_);
    return parser.parse();
}

Value ExpressionEvaluator::resolve_param(const std::string& value) const {
    if (value.starts_with('$')) {
        return evaluate(normalize_expr(std::string_view(value).substr(1)));
    }
    if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
        return Value::make_string(value.substr(1, value.size() - 2));
    }
    // numeric text (Rust parse::<i64>/<f64> full-string semantics)
    {
        const char* b = value.c_str();
        char* end = nullptr;
        errno = 0;
        const long long v = std::strtoll(b, &end, 10);
        if (errno == 0 && end != b && *end == '\0') return Value::make_int(int64_t(v));
        errno = 0;
        end = nullptr;
        const double d = std::strtod(b, &end);
        if (errno == 0 && end != b && *end == '\0') return Value::make_float(d);
    }
    return Value::make_string(value);
}

std::string ExpressionEvaluator::resolve_param_str(const std::string& value) const {
    if (value.starts_with('$')) {
        return evaluate(normalize_expr(std::string_view(value).substr(1))).to_string();
    }
    if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

// ---------------------------------------------------------------------------
// §3 instructions — tag parameter parser
// ---------------------------------------------------------------------------

std::map<std::string, std::string> parse_params(std::string_view params_str, size_t line) {
    std::map<std::string, std::string> params;
    size_t i = 0;
    const size_t n = params_str.size();
    size_t param_index = 0;

    auto skip_ws = [&]() {
        while (i < n && is_space(params_str[i])) ++i;
    };

    while (i < n) {
        skip_ws();
        if (i >= n) break;

        // Read key up to '=' or whitespace.
        const size_t key_start = i;
        while (i < n && params_str[i] != '=' && !is_space(params_str[i])) ++i;
        std::string key(params_str.substr(key_start, i - key_start));
        if (key.empty()) break;

        skip_ws();

        if (i >= n || params_str[i] != '=') {
            // No '=': positional value.
            params[std::to_string(param_index)] = std::move(key);
            ++param_index;
            continue;
        }
        ++i; // consume '='
        skip_ws();

        std::string value;
        if (i < n && params_str[i] == '"') {
            ++i; // opening quote
            bool closed = false;
            while (i < n) {
                const char c = params_str[i++];
                if (c == '\\' && i < n) {
                    // decode_asb escapes 值内 `"`/`\` 为 \" / \\,并把真实换行
                    // 写成 \n / \r;这里对称还原。其它 \x 组合保持字面(如 Lua
                    // 源码里的 \n 两字符序列先被解码成 \\n,再还原为 \n)。
                    const char nx = params_str[i];
                    if (nx == '"' || nx == '\\' || nx == 'n' || nx == 'r') {
                        value.push_back(nx == 'n' ? '\n' : nx == 'r' ? '\r' : nx);
                        ++i;
                    } else {
                        value.push_back(c);
                    }
                    continue;
                }
                if (c == '"') {
                    closed = true;
                    break;
                }
                value.push_back(c);
            }
            if (!closed) throw ParseError(line, "unterminated quote");
        } else {
            while (i < n && !is_space(params_str[i])) value.push_back(params_str[i++]);
        }
        params[std::move(key)] = std::move(value);
        ++param_index;
    }
    return params;
}

// ---------------------------------------------------------------------------
// §4 script text — .iet/.ast parser: labels, tags, [lua] blocks
// ---------------------------------------------------------------------------

namespace {

std::string trim(std::string_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

struct Segment {
    bool is_tag;     // tag: interior text (no brackets); text: story text
    std::string text;
};

// Applies this language's comment rules to one raw line, removing comment
// material from its code content:
//   - "//" comments to end of line, recognized only at the start of a text
//     run (start of the line, or just after a ']' tag close, with at most
//     whitespace before it) -- inside "[...]" parameters '/' is literal;
//   - ";" comments only start at the very beginning of a line's content
//     (checked by the caller);
//   - "/* ... */" block comments. Content inside a block is opaque: no
//     bracket/quote/comment parsing happens there, so a block may contain
//     '[' or "//" and may span raw lines. A block opened at the start of a
//     line (nothing but whitespace before it) may continue on following
//     lines (in_block state); a block opened right after a tag close that
//     is not closed on the same line just runs to the end of the line,
//     like "//".
// Quotes inside a tag protect their content; quotes outside tags are plain
// story text (matching split_line_segments).
std::string strip_line_comments(std::string_view raw, size_t line_no,
                                bool& in_block, size_t& block_line) {
    std::string out;
    if (in_block) {
        // Opaque block content: skip everything up to the first "*/".
        const size_t close = raw.find("*/");
        if (close == std::string_view::npos) return out;
        in_block = false;
        raw = raw.substr(close + 2); // code after the closer starts fresh
        if (raw.empty()) return out;
    }
    bool in_tag = false;
    bool in_quote = false;
    bool fresh = true;         // comment markers allowed here (text-run start)
    bool line_ws_only = true;  // only whitespace emitted so far on this line
    size_t i = 0;
    const size_t len = raw.size();
    while (i < len) {
        const char c = raw[i];
        if (c == '/' && !in_tag && i + 1 < len &&
            (raw[i + 1] == '/' || raw[i + 1] == '*') && fresh) {
            if (raw[i + 1] == '/') {
                return out; // line comment: the rest of the line is dropped
            }
            // "/*" block comment; find its closer (no nesting).
            const size_t close = raw.find("*/", i + 2);
            if (close == std::string_view::npos) {
                if (line_ws_only) {
                    in_block = true; // may span the following lines
                    block_line = line_no;
                }
                return out; // (else) unclosed inline block ends at EOL
            }
            i = close + 2;
            fresh = true; // content after the comment starts a fresh run
            continue;
        }
        out.push_back(c);
        if (c == '[' && !in_tag) {
            in_tag = true;
            line_ws_only = false;
        } else if (c == ']' && in_tag && !in_quote) {
            in_tag = false;
            fresh = true; // text run starts after the tag
        } else if (c == '"' && in_tag) {
            in_quote = !in_quote;
        } else if (!in_tag && !is_space(c)) {
            fresh = false;
            line_ws_only = false;
        }
        ++i;
    }
    return out;
}

// Mirrors  split_line_segments: splits a line into tag/text pieces.
// Quotes inside tags protect '[' and ']'; an unterminated "[..." tail is
// kept as story text with the bracket restored.
std::vector<Segment> split_line_segments(std::string_view line) {
    std::vector<Segment> segments;
    std::string current;
    bool in_tag = false;
    bool in_quote = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '\\' && in_tag && in_quote && i + 1 < line.size()) {
            // 引号内反斜杠转义(decode_asb 把值里的 `"`/`\` 写成 \" / \\):
            // 转义字符原样保留,不切换引号状态,后面的 ] 也不会被提前关闭。
            current.push_back(ch);
            current.push_back(line[++i]);
        } else if (ch == '"' && in_tag) {
            in_quote = !in_quote;
            current.push_back(ch);
        } else if (ch == '[' && !in_tag) {
            if (!current.empty()) {
                segments.push_back({false, std::move(current)});
                current.clear();
            }
            in_tag = true;
        } else if (ch == ']' && in_tag && !in_quote) {
            segments.push_back({true, std::move(current)});
            current.clear();
            in_tag = false;
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        if (in_tag) current.insert(current.begin(), '['); // restore bracket
        segments.push_back({false, std::move(current)});
    }
    return segments;
}

std::vector<std::string_view> split_lines(std::string_view content) {
    std::vector<std::string_view> lines;
    size_t pos = 0;
    while (pos <= content.size()) {
        const size_t nl = content.find('\n', pos);
        const size_t end = nl == std::string_view::npos ? content.size() : nl;
        std::string_view line = content.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
        pos = nl == std::string_view::npos ? content.size() + 1 : nl + 1;
    }
    return lines;
}

} // namespace

Script Script::parse(const std::string& name, std::string_view content) {
    Script out;
    out.name = name;

    const auto lines = split_lines(content);
    size_t line_idx = 0;
    const size_t n = lines.size();

    // State of a "/*" block comment opened at the start of a line; such a
    // block may span any number of raw lines until its "*/".
    bool in_block = false;
    size_t block_line = 0;

    while (line_idx < n) {
        // Pull the next effective line: blank, ";"-prefixed and comment
        // content (including whole "/* ... */" blocks) is consumed here, so
        // the remaining `line` holds code only. A block closer may leave
        // code on the same raw line, which is processed below.
        std::string line;
        while (true) {
            if (line_idx >= n) break;
            line = trim(strip_line_comments(lines[line_idx], line_idx + 1, in_block,
                                            block_line));
            if (line.empty()) {
                ++line_idx;
                continue;
            }
            break;
        }
        if (line.empty()) break; // content exhausted (unterminated block throws below)
        const size_t line_num = line_idx + 1;

        if (line.starts_with(';')) { // ";" full-line comment
            ++line_idx;
            continue;
        }
        if (line[0] == '*') {
            const std::string label = trim(std::string_view(line).substr(1));
            if (!label.empty()) out.labels[label] = out.instructions.size();
            ++line_idx;
            continue;
        }

        if (line.find('[') != std::string::npos) {
            bool lua_consumed = false;
            for (const auto& seg : split_line_segments(line)) {
                if (!seg.is_tag) {
                    const std::string text = trim(seg.text);
                    if (text.empty()) continue;
                    Instruction ins;
                    ins.kind = Instruction::Kind::Text; // not a reserved tag name
                    ins.params["text"] = text;
                    ins.line = line_num;
                    out.instructions.push_back(std::move(ins));
                    continue;
                }
                const std::string inner = trim(seg.text);
                if (inner == "lua") {
                    // Collect raw lines until "[/lua]" (content preserved).
                    std::string lua_code;
                    ++line_idx;
                    bool found_end = false;
                    while (line_idx < n) {
                        const std::string lua_line = trim(lines[line_idx]);
                        if (lua_line == "[/lua]") {
                            found_end = true;
                            ++line_idx;
                            break;
                        }
                        if (!lua_code.empty()) lua_code.push_back('\n');
                        lua_code.append(lines[line_idx]);
                        ++line_idx;
                    }
                    if (!found_end) throw ParseError(line_num, "missing [/lua] end marker");
                    Instruction ins;
                    ins.kind = Instruction::Kind::LuaBlock; // not a reserved tag name
                    ins.params["code"] = std::move(lua_code);
                    ins.line = line_num;
                    out.instructions.push_back(std::move(ins));
                    lua_consumed = true;
                    break; // [/lua] terminates the rest of the tag line
                }

                size_t sp = 0;
                while (sp < inner.size() && !is_space(inner[sp])) ++sp;
                const std::string tag = inner.substr(0, sp);
                if (tag.empty()) {
                    throw ParseError(line_num, "empty tag");
                }
                Instruction ins;
                ins.tag = tag;
                ins.params = parse_params(trim(std::string_view(inner).substr(sp)), line_num);
                ins.line = line_num;
                out.instructions.push_back(std::move(ins));
            }
            if (!lua_consumed) ++line_idx;
            continue;
        }

        Instruction ins;
        ins.kind = Instruction::Kind::Text; // not a reserved tag name
        ins.params["text"] = line;
        ins.line = line_num;
        out.instructions.push_back(std::move(ins));
        ++line_idx;
    }
    if (in_block) {
        throw ParseError(block_line, "unterminated block comment");
    }
    return out;
}

// ---------------------------------------------------------------------------
// §5 ASB decoder — binary script -> Artemis text
// ---------------------------------------------------------------------------

namespace {

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& d, size_t start) : data_(d), pos_(start) {}
    size_t remaining() const { return data_.size() - pos_; }
    uint8_t u8() {
        if (pos_ >= data_.size()) throw AsbError("unexpected end of file");
        return data_[pos_++];
    }
    uint32_t u32() {
        if (remaining() < 4) throw AsbError("unexpected end of file (u32)");
        const uint32_t v = uint32_t(data_[pos_]) | (uint32_t(data_[pos_ + 1]) << 8) |
                           (uint32_t(data_[pos_ + 2]) << 16) |
                           (uint32_t(data_[pos_ + 3]) << 24);
        pos_ += 4;
        return v;
    }
    std::string cstring(uint32_t len) {
        // mkmh 等新作 asb 的条目值可携带大段内嵌脚本(实测 5KB+);上限只作
        // 防错(长度字段损坏),remaining 检查已兜底,放宽到 1MB。
        if (len > (1u << 20)) throw AsbError("string too long in ASB");
        if (remaining() < size_t(len) + 1) throw AsbError("unexpected end of file (string)");
        std::string s(reinterpret_cast<const char*>(data_.data() + pos_), len);
        pos_ += len;
        if (data_[pos_] != 0) throw AsbError("missing NUL after ASB string");
        ++pos_;
        return s;
    }

private:
    const std::vector<uint8_t>& data_;
    size_t pos_ = 0;
};

} // namespace

namespace {
/// mkmh 等新作 ASB 会把编译期元信息以 \x0b (VT) 前缀编进条目名/参数键
/// (实测 "\x0bgoto"、"\x0bindex")。文本层把它们当普通名/键,解码时去掉
/// 前缀即可(值不受影响)。
std::string strip_vt(std::string s) {
    if (!s.empty() && s[0] == '\x0b') s.erase(0, 1);
    return s;
}
} // namespace

std::string decode_asb(const std::vector<uint8_t>& data) {
    if (data.size() < 9) throw AsbError("ASB file too short");
    if (!(data[0] == 'A' && data[1] == 'S' && data[2] == 'B' && data[3] == 0)) {
        throw AsbError("bad ASB magic");
    }
    Reader r(data, 4);
    r.u8(); // flag
    const uint32_t total = r.u32();
    std::string out;
    for (uint32_t i = 0; i < total; ++i) {
        if (r.remaining() < 8) throw AsbError("ASB entry header truncated");
        const uint32_t type = r.u32();
        const uint32_t name_len = r.u32();
        const std::string name = strip_vt(r.cstring(name_len));
        if (type == 1) {
            out += "*" + name + "\n";
        } else if (type == 0) {
            r.u32(); // serial (discarded)
            const uint32_t param_count = r.u32();
            std::string tag = "[" + name;
            for (uint32_t j = 0; j < param_count; ++j) {
                if (r.remaining() < 8) throw AsbError("ASB param header truncated");
                const uint32_t kl = r.u32();
                const std::string key = strip_vt(r.cstring(kl));
                const uint32_t vl = r.u32();
                const std::string val = r.cstring(vl);
                tag += " ";
                tag += key;
                tag += "=\"";
                for (const char c : val) {
                    // 值内 `"`/`\` 转义为 \" / \\;真实换行转义为 \n/\r,
                    // 否则会把标签行劈成两行(实测 macro.iet message 值含
                    // CRLF)。解析端 (parse_params) 对称地还原。
                    if (c == '"' || c == '\\') {
                        tag.push_back('\\');
                        tag.push_back(c);
                    } else if (c == '\n') {
                        tag += "\\n";
                    } else if (c == '\r') {
                        tag += "\\r";
                    } else {
                        tag.push_back(c);
                    }
                }
                tag += "\"";
            }
            tag += "]";
            out += tag + "\n";
        } else {
            throw AsbError("unknown ASB entry type " + std::to_string(type));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// §6 interpreter — execution, waits, tag dispatch, Lua hooks
// ---------------------------------------------------------------------------

namespace {

// ---- depth scans (mirror tags/ + interpreter helpers) ---------

/// Next [elseif]/[else]/[/if] at depth 0 after `from` (exclusive).
std::optional<size_t> find_else_elseif_or_endif(const Script& script, size_t from) {
    int depth = 0;
    for (size_t i = from + 1; i < script.instructions.size(); ++i) {
        const std::string& tag = script.instructions[i].tag;
        if (tag == "if") {
            ++depth;
        } else if (tag == "/if") {
            if (depth == 0) return i;
            --depth;
        } else if (depth == 0 && (tag == "elseif" || tag == "else")) {
            return i;
        }
    }
    return std::nullopt;
}

/// [/if] matching the [elseif]/[else] at `from`.
std::optional<size_t> find_matching_endif(const Script& script, size_t from) {
    int depth = 1;
    for (size_t i = from + 1; i < script.instructions.size(); ++i) {
        const std::string& tag = script.instructions[i].tag;
        if (tag == "if") {
            ++depth;
        } else if (tag == "/if") {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::nullopt;
}

/// [/loop] matching the [loop] at `from`.
std::optional<size_t> find_endloop(const Script& script, size_t from) {
    int depth = 0;
    for (size_t i = from + 1; i < script.instructions.size(); ++i) {
        const std::string& tag = script.instructions[i].tag;
        if (tag == "loop") {
            ++depth;
        } else if (tag == "/loop") {
            if (depth == 0) return i;
            --depth;
        }
    }
    return std::nullopt;
}

/// [loop] matching the [/loop] at `from` (scan backward).
std::optional<size_t> find_loop_start(const Script& script, size_t from) {
    int depth = 0;
    for (size_t i = from; i-- > 0;) {
        const std::string& tag = script.instructions[i].tag;
        if (tag == "/loop") {
            ++depth;
        } else if (tag == "loop") {
            if (depth == 0) return i;
            --depth;
        }
    }
    return std::nullopt;
}

} // namespace (scan helpers)

namespace {

/// Tags the engine dispatches natively (registry + special cases).
bool is_builtin_tag(const std::string& tag) {
    static const char* kBuiltins[] = {
        "jump",  "goto",   "call",   "return", "stop",   "wt",    "wt0",     "wait",
        "exkey",
        "@",     "var",    "if",     "elseif", "else",  "/if",     "loop",  "/loop",
        "lua",   "reset", "exit",   "gotitle", "yesno", "dialog", "calllua", "macroadd",
        "macrodel", "tag",
    };
    for (const char* b : kBuiltins) {
        if (tag == b) return true;
    }
    return false;
}

} // namespace

const Script* Interpreter::get_script(const std::string& name) const {
    const auto it = scripts_.find(name);
    return it == scripts_.end() ? nullptr : &it->second;
}

void Interpreter::load_script(const std::string& name, std::string_view text) {
    Script script = Script::parse(name, text);
    // [lua] blocks execute once at load, in order (Artemis semantics).
    for (size_t i = 0; i < script.instructions.size(); ++i) {
        const auto& ins = script.instructions[i];
        if (ins.kind != Instruction::Kind::LuaBlock) continue;
        const auto key = std::make_pair(name, i);
        if (executed_lua_blocks_.count(key)) continue;
        if (!hooks_.run_lua_block) continue; // engine not wired; defer (补跑 at step)
        const std::string* code = ins.get("code");
        hooks_.run_lua_block(code ? *code : "", name, i);
        executed_lua_blocks_[key] = true;
    }
    scripts_[name] = std::move(script);
}

void Interpreter::load_file(const std::string& name, const std::vector<uint8_t>& bytes) {
    if (bytes.size() >= 4 && bytes[0] == 'A' && bytes[1] == 'S' && bytes[2] == 'B' &&
        bytes[3] == 0) {
        // ASB -> text -> regular parser
        load_script(name, decode_asb(bytes));
        return;
    }
    const std::string text = util::decode_to_utf8(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
        config_.charset);
    load_script(name, text);
}

void Interpreter::load_external_script(const std::string& file) {
    if (scripts_.count(file)) return;
    if (!hooks_.file_loader) {
        throw ScriptError("script-not-found", "no file loader wired; cannot load " + file);
    }
    auto bytes = hooks_.file_loader(file);
    if (!bytes) {
        throw ScriptError("script-not-found", "file not found: " + file);
    }
    load_file(file, *bytes);
}

std::vector<Instruction> Interpreter::take_tag_queue() {
    std::vector<Instruction> out;
    out.swap(tag_queue_);
    return out;
}

void Interpreter::restore_tag_queue(std::vector<Instruction> pending) {
    // Anything still queued (tags generated by the flushed handler) keeps its
    // position in FRONT of the restored pending continuation (
    // fire_save_handler_and_flush: generated_leftovers.extend(pending)).
    std::vector<Instruction> leftovers;
    leftovers.swap(tag_queue_);
    tag_queue_ = std::move(leftovers);
    tag_queue_.insert(tag_queue_.end(), pending.begin(), pending.end());
}

bool Interpreter::try_load_script(const std::string& file, std::string* error) {
    if (file.empty() || scripts_.count(file)) return true;
    try {
        load_external_script(file);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

void Interpreter::restore_position(const std::string& script, size_t line,
                                   const std::vector<CallFrame> stack) {
    if (!script.empty() && !scripts_.count(script)) {
        load_external_script(script);
    }
    if (script.empty()) {
        current_script_name_.reset();
    } else {
        current_script_name_ = script;
    }
    current_line_ = line;
    call_stack_ = stack;
    arrived_by_jump_ = false;
    last_wait_from_queue_ = false;
}

void Interpreter::start(const std::string& script, std::string_view label) {
    const auto it = scripts_.find(script);
    if (it == scripts_.end()) {
        throw ScriptError("script-not-found", "script not loaded: " + script);
    }
    const auto line = it->second.get_label_line(label);
    if (!line) {
        throw ScriptError("label", "label not found: " + std::string(label));
    }
    current_script_name_ = script;
    current_line_ = *line;
    call_stack_.clear();
    arrived_by_jump_ = false;
}

void Interpreter::boot(const std::string& script) {
    load_external_script(script);
    // Default macro file; missing is normal, ignore silently.
    if (hooks_.file_loader) (void)hooks_.file_loader("macro.iet");
    const Script* s = get_script(script);
    if (!s) throw ScriptError("script-not-found", "boot script missing: " + script);
    std::optional<size_t> line;
    for (const char* lbl : {"main", "start", "_start"}) {
        if (s->labels.count(lbl)) {
            line = s->labels.at(lbl);
            break;
        }
    }
    current_script_name_ = script;
    current_line_ = line.value_or(0);
    call_stack_.clear();
    arrived_by_jump_ = false;
}

const std::string* Interpreter::current_script() const {
    return current_script_name_ ? &*current_script_name_ : nullptr;
}

void Interpreter::next_line() {
    wait_reason_info_.clear();
    last_wait_event_.reset();
    if (last_wait_from_queue_) {
        last_wait_from_queue_ = false;
        return;
    }
    current_line_ += 1;
}

void Interpreter::enqueue_tag(std::string tag, std::map<std::string, std::string> params) {
    Instruction ins;
    ins.tag = std::move(tag);
    ins.params = std::move(params);
    ins.line = 0;
    tag_queue_.push_back(std::move(ins));
}

ExecutionResult Interpreter::run_queued() {
    queued_saw_call_ = false;
    queued_saw_jump_ = false;
    // Only the queue drain (flush_tag_queue) — never inline script stepping.
    // Pauses are reported exactly like the run-entry drain, including the
    // last_wait_from_queue_ bookkeeping that makes next_line a no-op for
    // queue-sourced waits.
    if (const auto wait_result = flush_tag_queue()) return *wait_result;
    return ExecutionResult::Completed;
}

// ---------------------------------------------------------------------------
// Tag execution (control tags mirroring tags/ + tags/)
// ---------------------------------------------------------------------------

namespace {

/// One tag's outcome. Wait/Emit events flow through the callback inside run.
enum class TagOutcomeKind {
    Continue,
    Jump,       // line jump within current script
    JumpExternal, // file+label jump
    Call,       // push frame then jump
    Return,
    WaitEvent,  // emit event through callback (Wait semantics)
    CustomEvent, // unknown tag -> Event::Custom
};

struct TagOutcome {
    TagOutcomeKind kind = TagOutcomeKind::Continue;
    Event event;
    size_t line = 0;
    std::string file;
    std::string label;
    size_t return_line = 0;
    std::string return_script;
};

bool tag_bool(std::string_view v, bool fallback) {
    (void)fallback; // TODO: unused; kept for parse_bool parity
    std::string s(v);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s == "1" || s == "true" || s == "on" || s == "yes";
}

} // namespace

/// Entry point for one instruction.
static TagOutcome execute_tag(Interpreter& it, const Instruction& ins, bool apply_filter) {
    const std::string& tag = ins.tag;
    VariableStore& vars = it.variables();
    const ExpressionEvaluator eval(vars);
    TagOutcome out;
    const Script* cur_script = it.get_script(*it.current_script());
    const size_t here = it.current_line();

    auto resolve_or_empty = [&](std::string_view key) -> std::string {
        const std::string* v = ins.get(key);
        return v ? *v : "";
    };

    // -- inline Lua tag filter (e:setTagFilter): consult tags.<tag> --------
    // tags with a NATIVE engine handler (register_engine_tag, e.g.
    // [alldelete]) are exempt — these dispatch natively and
    // never lets a Lua framework handler swallow them (FPM registers
    // tags.alldelete in image.lua, whose Lua-side lydel bookkeeping cannot
    // outlive [exit]/[reset]; the native whole-scene fade+clear is the
    // engine contract).
    // mkmh(2023+) 框架注册了 tags.return,但其实现依赖
    // getScriptStack/getScriptBlock 等脚本块 API(引擎按等待/调用栈模型
    // 原生执行,块栈是存根),先询问会在第一个 [return] 上崩 —— 仅豁免
    // 这一个内置流标签。其余标签保持老引擎语义:先问 Lua tag filter
    // (fpm/NekoMiko/thyt 的 tags.stop(exskip)/tags.@ / exkey / タイトル
    // … 仍由过滤器拦截)。
    if (apply_filter && tag != "return") {
        const bool is_engine_native = it.engine_tags().count(tag) > 0;
        const bool has_builtin = is_builtin_tag(tag) || is_engine_native;
        if (!is_engine_native) {
            oa::runtime::FilterDecision d = oa::runtime::FilterDecision::Missing;
            try {
                d = it.lua_bridge().run_tag_filter(tag, ins.params);
            } catch (const oa::runtime::LuaError& e) {
                // research/130: a broken tags.<tag> handler no longer kills the
                // app; the filter verdict degrades to "not handled by the
                // script", so the engine's own native branch (if any) runs —
                // the original runtime's tag dispatch logs and continues.
                it.lua_bridge().report_dispatch_error("tag filter", tag, e.what());
                d = oa::runtime::FilterDecision::Missing;
            }
            if (d == oa::runtime::FilterDecision::Consume ||
                (!has_builtin && d != oa::runtime::FilterDecision::Missing)) {
                return out; // consumed by Lua
            }
        }
    }

    if (tag == "calllua") {
        const std::string raw = resolve_or_empty("function");
        if (raw.empty()) {
            throw ScriptError("runtime", "calllua missing function parameter", ins.line);
        }
        const std::string function = eval.resolve_param_str(raw);
        if (function.empty()) return out; // optional callback -> silent
        std::map<std::string, std::string> extra;
        for (const auto& [k, v] : ins.params) {
            if (k != "function") extra[k] = eval.resolve_param(v).to_string();
        }
        const auto& hook = it.hooks().call_lua_function;
        if (!hook) {
            throw ScriptError("lua", "calllua: lua engine not wired", ins.line);
        }
        // Non-empty name that resolves to nothing is a silent no-op in the
        // (optional callbacks are pervasive in game Lua).
        (void)hook(function, extra, *it.current_script(), ins.line);
        return out;
    }
    if (tag == "lua") {
        if (const std::string* code = ins.get("script"); code && !code->empty()) {
            if (!it.hooks().run_lua_block) {
                throw ScriptError("lua", "lua: engine not wired", ins.line);
            }
            it.hooks().run_lua_block(*code, *it.current_script(), ins.line);
        }
        return out;
    }
    if (tag == "macroadd" || tag == "macrodel") {
        // Macro registry arrives with the Lua layer; loading bookkeeping
        // only. File read errors are debug-level.
        return out;
    }

    // -- engine tags (runtime-registered overrides; e.g. [reset]) ----------
    {
        const auto& engine_tags = it.engine_tags();
        if (const auto et = engine_tags.find(tag); et != engine_tags.end()) {
            auto ev = et->second(it, ins);
            if (ev) {
                out.kind = TagOutcomeKind::WaitEvent;
                out.event = *ev;
            }
            return out;
        }
    }

    if (tag == "jump" || tag == "goto") {
        // ASB 编译形态的 [goto index=N] / [jump index=N](mkmh 等新作):
        // 无条件跳到本脚本第 N 条指令(decode 后 N == 指令序号)。
        if (const std::string* ip = ins.get("index")) {
            char* end = nullptr;
            const long long n = std::strtoll(ip->c_str(), &end, 10);
            if (end == ip->c_str() || *end != '\0' || n < 0 ||
                size_t(n) >= cur_script->instructions.size()) {
                throw ScriptError("runtime", tag + ": bad index target: " + *ip, ins.line);
            }
            out.kind = TagOutcomeKind::Jump;
            out.line = size_t(n);
            return out;
        }
        const std::string label = resolve_or_empty("label");
        if (const std::string* cond = ins.get("cond")) {
            const Value v = eval.resolve_param("$" + *cond);
            if (!v.to_bool()) return out;
        }
        if (const std::string* file = ins.get("file")) {
            out.kind = TagOutcomeKind::JumpExternal;
            out.file = eval.resolve_param_str(*file);
            out.label = label;
            return out;
        }
        const auto line = cur_script->get_label_line(label);
        if (!line) throw ScriptError("label", "jump: label not found: " + label, ins.line);
        out.kind = TagOutcomeKind::Jump;
        out.line = *line;
        return out;
    }
    if (tag == "call") {
        out.kind = TagOutcomeKind::Call;
        out.file = eval.resolve_param_str(resolve_or_empty("file"));
        out.label = resolve_or_empty("label");
        out.return_line = it.current_line() + 1;
        out.return_script = *it.current_script();
        return out;
    }
    if (tag == "return") {
        out.kind = TagOutcomeKind::Return;
        return out;
    }
    if (tag == "stop") {
        WaitReason r;
        r.kind = WaitReason::Kind::Stop;
        r.id = resolve_or_empty("0");
        out.kind = TagOutcomeKind::WaitEvent;
        out.event = Event::wait(std::move(r));
        return out;
    }
    if (tag == "wt") {
        WaitReason r;
        r.kind = WaitReason::Kind::Timed;
        r.milliseconds =
            uint64_t(eval.resolve_param(resolve_or_empty("time")).as_int().value_or(0));
        r.input = eval.resolve_param(resolve_or_empty("input")).as_int().value_or(1);
        out.kind = TagOutcomeKind::WaitEvent;
        out.event = Event::wait(std::move(r));
        return out;
    }
    if (tag == "wt0") {
        WaitReason r;
        r.kind = WaitReason::Kind::Generic0;
        out.kind = TagOutcomeKind::WaitEvent;
        out.event = Event::wait(std::move(r));
        return out;
    }
    if (tag == "wait") {
        WaitReason r;
        r.kind = WaitReason::Kind::Timed;
        const uint64_t time =
            uint64_t(eval.resolve_param(resolve_or_empty("time")).as_int().value_or(0));
        r.milliseconds = time;
        r.input = eval.resolve_param(resolve_or_empty("input")).as_int().value_or(0);
        const std::string video = resolve_or_empty("video");
        const std::string scenario = resolve_or_empty("scenario");
        const std::string se = resolve_or_empty("se");
        if (!video.empty()) {
            r.kind = WaitReason::Kind::VideoLayer;
            r.id = video;
        } else if (int m = 0; (m = static_cast<int>(eval.resolve_param(scenario).as_int().value_or(0))),
                   m == 1 || m == 2) {
            r.kind = WaitReason::Kind::ScenarioTween;
            r.mode = m;
        } else if (!se.empty()) {
            r.kind = WaitReason::Kind::Se;
            r.id = se;
            if (ins.has("time")) {
                r.milliseconds = time;
                r.time_given = true; // counts from the SE play start
            }
        }
        out.kind = TagOutcomeKind::WaitEvent;
        out.event = Event::wait(std::move(r));
        return out;
    }
    if (tag == "exkey") {
        WaitReason r;
        r.kind = WaitReason::Kind::KeyWait;
        if (const std::string* b = ins.get("btn")) r.buttons.push_back(*b);
        for (int i = 0; i < 10; ++i) {
            if (const std::string* b = ins.get(std::to_string(i))) r.buttons.push_back(*b);
        }
        out.kind = TagOutcomeKind::WaitEvent;
        out.event = Event::wait(std::move(r));
        return out;
    }
    if (tag == "@") {
        WaitReason r;
        r.kind = WaitReason::Kind::Generic;
        out.kind = TagOutcomeKind::WaitEvent;
        out.event = Event::wait(std::move(r));
        return out;
    }
    if (tag == "var") {
        it.apply_var(ins.params);
        return out;
    }
    if (tag == "if" || tag == "elseif") {
        const std::string est = resolve_or_empty("estimate");
        const Value v = eval.resolve_param(est.empty() ? "1" : est);
        if (v.to_bool()) return out; // Continue into body
        // ASB 编译形态(mkmh 等新作):if/elseif 带 index=N —— 条件为假时
        // 直接跳到本脚本第 N 条指令(decode 后 N == 指令序号),无需结构扫描。
        if (const std::string* ip = ins.get("index")) {
            char* end = nullptr;
            const long long n = std::strtoll(ip->c_str(), &end, 10);
            if (end == ip->c_str() || *end != '\0' || n < 0 ||
                size_t(n) >= cur_script->instructions.size()) {
                throw ScriptError("runtime", tag + ": bad index target: " + *ip, ins.line);
            }
            out.kind = TagOutcomeKind::Jump;
            out.line = size_t(n);
            return out;
        }
        const auto target = find_else_elseif_or_endif(*cur_script, here);
        if (!target) throw ScriptError("runtime", tag + ": missing else/elseif//if", ins.line);
        out.kind = TagOutcomeKind::Jump;
        out.line = cur_script->instructions[*target].tag == "else" ? *target + 1 : *target;
        return out;
    }
    if (tag == "else") {
        const auto endif = find_matching_endif(*cur_script, here);
        if (!endif) throw ScriptError("runtime", "else: missing /if", ins.line);
        out.kind = TagOutcomeKind::Jump;
        out.line = *endif;
        return out;
    }
    if (tag == "/if") {
        return out;
    }
    if (tag == "loop") {
        const std::string est = resolve_or_empty("estimate");
        const Value v = eval.resolve_param(est.empty() ? "1" : est);
        if (v.to_bool()) return out;
        if (const std::string* ip = ins.get("index")) {
            char* end = nullptr;
            const long long n = std::strtoll(ip->c_str(), &end, 10);
            if (end == ip->c_str() || *end != '\0' || n < 0 ||
                size_t(n) >= cur_script->instructions.size()) {
                throw ScriptError("runtime", tag + ": bad index target: " + *ip, ins.line);
            }
            out.kind = TagOutcomeKind::Jump;
            out.line = size_t(n);
            return out;
        }
        const auto end = find_endloop(*cur_script, here);
        if (!end) throw ScriptError("runtime", "loop: missing /loop", ins.line);
        out.kind = TagOutcomeKind::Jump;
        out.line = *end + 1;
        return out;
    }
    if (tag == "/loop") {
        const auto start = find_loop_start(*cur_script, here);
        if (!start) throw ScriptError("runtime", "/loop: missing loop", ins.line);
        out.kind = TagOutcomeKind::Jump;
        out.line = *start;
        return out;
    }
    if (tag == "reset") {
        // Builtin [reset] (engine may override via register_engine_tag).
        vars.reset_local_temp();
        Event e;
        e.kind = Event::Kind::Reset;
        out.kind = TagOutcomeKind::WaitEvent;
        out.event = std::move(e);
        return out;
    }
    if (tag == "exit") {
        Event e;
        e.kind = Event::Kind::Exit;
        out.kind = TagOutcomeKind::WaitEvent;
        out.event = std::move(e);
        return out;
    }
    if (tag == "gotitle") {
        Event e;
        e.kind = Event::Kind::GoTitle;
        out.kind = TagOutcomeKind::WaitEvent;
        out.event = std::move(e);
        return out;
    }
    // ---- M5b: native tag events (string-preserving passthrough) ----
    {
        auto set_event = [&](Event::Kind k) {
            out.kind = TagOutcomeKind::WaitEvent;
            out.event.kind = k;
            out.event.tag = tag;
            out.event.params = ins.params;
            out.event.id = ins.get_or("id", "");
        };
        auto set_line = [&](Event::Kind k, const std::string& content) {
            out.kind = TagOutcomeKind::WaitEvent;
            out.event.kind = k;
            out.event.tag = tag;
            out.event.params = ins.params;
            out.event.content = content;
        };
        if (tag == "lyc" || tag == "lyc2") {
            set_event(Event::Kind::LayerCreate);
            return out;
        }
        if (tag == "lydel") {
            set_event(Event::Kind::LayerDelete);
            return out;
        }
        if (tag == "lyprop") {
            set_event(Event::Kind::LayerSetProps);
            return out;
        }
        if (tag == "lyevent" || tag == "lytween" || tag == "lytweendel" ||
            tag == "tweenset" || tag == "/tweenset" || tag == "anime" ||
            tag == "video") {
            set_event(Event::Kind::LayerEventCmd);
            return out;
        }
        // [trans] / [flip] are typed events: the runtime pauses the script on
        // non-zero [trans] (Stop{trans} wait) and the host renders the fade;
        // [flip] clears any in-flight transition.
        if (tag == "trans") {
            set_event(Event::Kind::Trans);
            return out;
        }
        if (tag == "flip") {
            set_event(Event::Kind::Flip);
            return out;
        }
        if (tag == "print") {
            set_line(Event::Kind::ScenarioLine, resolve_or_empty("data"));
            return out;
        }
        if (tag == "rt") {
            set_line(Event::Kind::LineBreak, "");
            return out;
        }
        if (tag == "rp") {
            set_line(Event::Kind::PageBreak, "");
            return out;
        }
        if (tag == "chgmsg") {
            set_event(Event::Kind::MessageLayerSwitch);
            return out;
        }
        if (tag == "/chgmsg" || tag == "chgmsg_close") {
            set_event(Event::Kind::MessageLayerPop);
            return out;
        }
        if (tag == "font" || tag == "font_close" || tag == "/font" ||
            tag == "fontdefault" || tag == "fontinit" || tag == "ruby" ||
            tag == "/ruby" || tag == "link" || tag == "/link" || tag == "glyph" ||
            tag == "scetween" || tag == "scein" || tag == "sceout" ||
            tag == "indent" || tag == "prohibit" || tag == "wordparts" ||
            tag == "rt2" || tag == "tximg" || tag == "txkey" || tag == "txnc"||
            tag == "backlog" || tag == "writebacklog") {
            // Resolve params through expression evaluator where  does:
            // FontHandler/FontDefaultHandler — resolve_param on all params
            // ScetweenHandler — resolve_param on all params
            // RubyHandler — resolve_param_str on "text" (preserves trailing zeros)
            if (tag == "font" || tag == "fontdefault" || tag == "scetween") {
                std::map<std::string, std::string> resolved;
                for (const auto& [k, v] : ins.params) {
                    resolved[k] = eval.resolve_param(v).to_string();
                }
                set_event(Event::Kind::TextConfig);
                out.event.params = std::move(resolved);
            } else if (tag == "ruby") {
                set_event(Event::Kind::TextConfig);
                auto text_it = out.event.params.find("text");
                if (text_it != out.event.params.end()) {
                    text_it->second = eval.resolve_param_str(text_it->second);
                }
            } else {
                set_event(Event::Kind::TextConfig);
            }
            return out;
        }
        // Remaining runtime-confirmed native tags: config/audio/UI markers.
        static const char* kConfigTags[] = {
            "skip", "automode", "autosave", "alreadyread",
            "hide", "rclick", "keyconfig", "mouse", "caption",
            "exec", "save", "delonpush", "setonpush", "setonclick",
            "setondrag", "setondragin", "setondragout", "setonrollover",
            "setonrollout", "delonclick", "delondrag", "delondragin",
            "delondragout", "delonrollover", "delonrollout",
            "setonautomodein", "setonautomodeout", "setonbacklogin",
            "setonbacklogout", "setoncommandskipin", "setoncommandskipout",
            "setoncontrolskipin", "setoncontrolskipout", "setondirchg",
            "setonhidein", "setonhideout", "setonwindowbutton",
            "delonautomodein", "delonautomodeout", "delonbacklogin",
            "delonbacklogout", "deloncommandskipin", "deloncommandskipout",
            "deloncontrolskipin", "deloncontrolskipout", "delondirchg",
            "delonhidein", "delonhideout", "delonwindowbutton",
            "splay", "sstop", "sfade", "span", "sxfade", "seplay", "sestop",
            "sefade", "sepan", "voice", "/voice", "setonsoundfinish",
            "delonsoundfinish", "sefadein", "sefadeout", "sfadein",
            "sfadeout", "allsoundstop", "loading", "saving", "loadmask",
            "repeatedly", "autoskip_disable", "sysshow", "syshide",
            "se_saveok", "se_loadok", "se_exitok", "se_ok",
            "lyshader", "takess", "savess", "linkdisable", "linkenable",
            "exkey",
        };
        for (const char* c : kConfigTags) {
            if (tag == c) {
                set_event(Event::Kind::ConfigEvent);
                return out;
            }
        }
    }

    if (tag == "yesno" || tag == "dialog") {
        // The host dialog/yesno/textfield widget is a host-side surface the
        // engine does not draw itself.
        // Until a real host UI exists, complete dialogs LOGICALLY so
        // real-game flows never hang (same fallback philosophy as the video
        // engine's logical immediate-finish):
        //   - textfield=VAR: store "" (empty input) into the variable the
        //     dialog would return; Artemis games read it back with e:var
        //     right after the tag (NekoMiko gamestart: myname), and the
        //     stream advances synchronously, so the next tag ([nameset])
        //     already observes the written variable.
        //   - notice (title/message only): auto-OK.
        //   - [yesno]: affirm (no NekoMiko script uses the native yesno tag
        //     today; revisit when the input surface lands).
        if (const std::string* tf = ins.get("textfield"); tf && !tf->empty()) {
            const std::string name = eval.resolve_param_str(*tf);
            if (!name.empty()) vars.set(name, Value::make_string(""));
        }
        return out;
    }

    // Unknown tag -> Event::Custom (fallback).
    out.kind = TagOutcomeKind::CustomEvent;
    out.event = Event::custom(tag, ins.params);
    return out;
}

// ---------------------------------------------------------------------------
// Run loop (interpreter main step loop)
// ---------------------------------------------------------------------------

ExecutionResult Interpreter::run() {
    for (;;) {
        // Drain tags queued by Lua (e:tag/e:enqueueTag) before inline steps.
        if (const auto wait_result = flush_tag_queue()) return *wait_result;

        last_wait_from_queue_ = false;

        const bool arrived_by_jump = std::exchange(arrived_by_jump_, false);

        if (!current_script_name_) return ExecutionResult::Completed;
        const auto sit = scripts_.find(*current_script_name_);
        if (sit == scripts_.end()) return ExecutionResult::Completed;
        const Script& script = sit->second;
        if (current_line_ >= script.instructions.size()) return ExecutionResult::Completed;
        const Instruction ins = script.instructions[current_line_];
        if (on_step) on_step(*current_script_name_, current_line_, ins);

        // if-chain fallthrough: sequential arrival at [elseif]/[else] means a
        // branch already ran; skip to the matching [/if].
        if (!arrived_by_jump &&
            (ins.tag == "elseif" || ins.tag == "else")) {
            const auto endif = find_matching_endif(script, current_line_);
            if (!endif) throw ScriptError("runtime", "unmatched elseif/else", ins.line);
            current_line_ = *endif;
            continue;
        }

        // Scenario story text.
        if (ins.kind == Instruction::Kind::Text) {
            const std::string text = ins.get_or("text", "");
            const Event event = Event::scenario_text(text);
            const CallbackResult cr = callback_(event);
            if (cr == CallbackResult::Continue) {
                ++current_line_;
                continue;
            }
            if (cr == CallbackResult::Pause) return ExecutionResult::Wait;
            throw ScriptError("aborted", "callback aborted", ins.line);
        }

        // [lua] blocks: executed once at load; stepping skips (补跑 fallback).
        if (ins.kind == Instruction::Kind::LuaBlock) {
            const auto key = std::make_pair(*current_script_name_, current_line_);
            if (!executed_lua_blocks_.count(key)) {
                if (!hooks_.run_lua_block) {
                    throw ScriptError("lua", "lua engine not wired; [lua] block pending",
                                      ins.line);
                }
                const std::string* code = ins.get("code");
                hooks_.run_lua_block(code ? *code : "", *current_script_name_, current_line_);
                executed_lua_blocks_[key] = true;
            }
            ++current_line_;
            continue;
        }

        if (std::getenv("OA_SELTRACE")) {
            const std::string* lf = ins.get("label");
            if ((ins.tag == "jump" || ins.tag == "call") && lf && *lf == "select")
                std::fprintf(stderr,
                             "[seltrace] %s->select @ %s:%zu pos=%s:%zu queue=%zu\n",
                             ins.tag.c_str(), current_script_name_
                                 ? current_script_name_->c_str() : "?",
                             ins.line, current_script_name_
                                 ? current_script_name_->c_str() : "?",
                             current_line_, tag_queue_.size());
        }
        const TagOutcome outcome = execute_tag(*this, ins, true);

        switch (outcome.kind) {
            case TagOutcomeKind::Continue:
                ++current_line_;
                continue;
            case TagOutcomeKind::Jump:
                arrived_by_jump_ = true;
                current_line_ = outcome.line;
                continue;
            case TagOutcomeKind::JumpExternal: {
                // load target then resolve label (empty label = file start)
                load_external_script(outcome.file);
                const Script* t = get_script(outcome.file);
                const auto line = t->get_label_line(outcome.label);
                if (!line) {
                    throw ScriptError("label", "jump: label not found: " + outcome.label,
                                      ins.line);
                }
                current_script_name_ = outcome.file;
                current_line_ = *line;
                arrived_by_jump_ = true;
                continue;
            }
            case TagOutcomeKind::Call: {
                call_stack_.push_back(CallFrame{outcome.return_script, outcome.return_line});
                if (std::getenv("OA_SELTRACE") &&
                    (outcome.return_script == "system/script.asb" ||
                     (current_script_name_ &&
                      *current_script_name_ == "system/script.asb"))) {
                    std::fprintf(stderr,
                                 "[seltrace] INLINE-CALL ret=%s:%zu origin=%s:%zu "
                                 "label=%s depth=%zu\n",
                                 outcome.return_script.c_str(), outcome.return_line,
                                 current_script_name_ ? current_script_name_->c_str() : "?",
                                 current_line_,
                                 ins.get_or("label", "-").c_str(),
                                 call_stack_.size());
                    if (call_stack_.size() >= 5) {
                        std::string cst;
                        for (const auto& fr : call_stack_) {
                            cst += fr.script + ":" +
                                   std::to_string(fr.return_line) + " ";
                        }
                        std::fprintf(stderr, "[seltrace]   INLINE-STACK [%s]\n",
                                     cst.c_str());
                    }
                }
                const std::string target_script = outcome.file.empty() ? *current_script_name_
                                                                       : outcome.file;
                if (!outcome.file.empty()) load_external_script(target_script);
                const Script* t = get_script(target_script);
                const auto line = t->get_label_line(outcome.label);
                if (!line) {
                    throw ScriptError("label", "call: label not found: " + outcome.label,
                                      ins.line);
                }
                current_script_name_ = target_script;
                current_line_ = *line;
                arrived_by_jump_ = true;
                continue;
            }
            case TagOutcomeKind::Return: {
                if (!call_stack_.empty()) {
                    const std::string origin_script =
                        current_script_name_ ? *current_script_name_ : "";
                    const size_t origin_line = current_line_;
                    const CallFrame frame = call_stack_.back();
                    call_stack_.pop_back();
                    current_script_name_ = frame.script;
                    current_line_ = frame.return_line;
                    // resurrection probe: a [return]
                    // landing on the game's select-[stop] area
                    // (system/script.asb line <= 1) re-parks a consumed
                    // choice park (482 crash family). Print the return's
                    // origin and the popped frame.
                    if (std::getenv("OA_SELTRACE") &&
                        (origin_script == "system/script.asb" ||
                         frame.script == "system/script.asb" ||
                         (current_script_name_ &&
                          *current_script_name_ == "system/script.asb"))) {
                        std::string cst;
                        for (const auto& fr : call_stack_) {
                            cst += fr.script + ":" + std::to_string(fr.return_line) + " ";
                        }
                        std::fprintf(stderr,
                                     "[seltrace] RET origin=%s:%zu popped=%s:%zu "
                                     "now=%s:%zu stack[%s]\n",
                                     origin_script.c_str(), origin_line,
                                     frame.script.c_str(), frame.return_line,
                                     current_script_name_
                                         ? current_script_name_->c_str()
                                         : "?",
                                     current_line_, cst.c_str());
                    }
                    continue;
                }
                return ExecutionResult::Completed;
            }
            case TagOutcomeKind::WaitEvent:
            case TagOutcomeKind::CustomEvent: {
                const CallbackResult cr = callback_(outcome.event);
                if (cr == CallbackResult::Continue) {
                    ++current_line_;
                    continue;
                }
                if (cr == CallbackResult::Pause) {
                    note_wait_reason(outcome.event);
                    // Same bookkeeping as the queue-drain path below: the host
                    // wait machine reads last_wait_event_ to classify the
                    // pause (Wait_ vs Trans vs media).
                    last_wait_event_ = outcome.event;
                    return ExecutionResult::Wait;
                }
                throw ScriptError("aborted", "callback aborted", ins.line);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Lua integration (single shared VM wiring)
// ---------------------------------------------------------------------------

// 按 Artemis 日期格式串渲染本地时间分量（ format_update_time）。
// yyyy=四位年 / yy=两位年 / MM|dd|hh|mm|ss=补零 / M|d|h|m|s=不补零；其余原样。
std::string format_update_time(const std::array<int64_t, 6>& c, const std::string& format) {
    std::string out;
    size_t i = 0;
    const auto pad2 = [](int64_t v) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02lld", (long long)v);
        return std::string(buf);
    };
    while (i < format.size()) {
        const char ch = format[i];
        size_t run = 1;
        while (i + run < format.size() && format[i + run] == ch) ++run;
        switch (ch) {
            case 'y':
                if (run >= 4) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "%04lld", (long long)c[0]);
                    out += buf;
                    i += run;
                } else if (run >= 2) {
                    out += pad2(c[0] % 100);
                    i += run;
                } else {
                    out += ch;
                    i += 1;
                }
                break;
            case 'M':
                out += (run >= 2) ? pad2(c[1]) : std::to_string(c[1]);
                i += (run >= 2) ? run : 1;
                break;
            case 'd':
                out += (run >= 2) ? pad2(c[2]) : std::to_string(c[2]);
                i += (run >= 2) ? run : 1;
                break;
            case 'h':
                out += (run >= 2) ? pad2(c[3]) : std::to_string(c[3]);
                i += (run >= 2) ? run : 1;
                break;
            case 'm':
                out += (run >= 2) ? pad2(c[4]) : std::to_string(c[4]);
                i += (run >= 2) ? run : 1;
                break;
            case 's':
                out += (run >= 2) ? pad2(c[5]) : std::to_string(c[5]);
                i += (run >= 2) ? run : 1;
                break;
            default:
                out += ch;
                i += 1;
                break;
        }
    }
    return out;
}

Interpreter::Interpreter() : Interpreter(Config{}) {}

Interpreter::Interpreter(const Config& config) : config_(config) {
    // engine seeds
    seed_engine_variables_impl();
    variables_.platform = config_.platform;
    lua_bridge_.reset(new oa::runtime::LuaBridge(build_lua_host()));
    // wire Lua hooks so [lua] blocks / calllua reach the shared VM
    hooks_.run_lua_block = [this](const std::string& code, const std::string& script_name,
                                  size_t line) {
        (void)line;
        try {
            lua_bridge().run_code(code, script_name);
        } catch (const oa::runtime::LuaError& e) {
            // research/130: an error in a [lua] block abandons the block (the
            // original runtime logs and continues); ScriptError would abort
            // the app/tick.
            lua_bridge().report_dispatch_error("lua block", script_name, e.what());
        }
    };
    hooks_.call_lua_function = [this](const std::string& function,
                                      const std::map<std::string, std::string>& params,
                                      const std::string& script_name, size_t line) {
        // OA_SELTRACE: every select_exit chain entry
        // executed from the engine side (script.asb *select_exit
        // [calllua select_exittrans/select_clicknext]) prints the engine
        // position + pending queue/call-stack depth, so a user crash log
        // pins down which chain reached clicknext without scr.select.
        if (std::getenv("OA_SELTRACE") &&
            (function == "select_clicknext" || function == "select_exittrans")) {
            std::fprintf(stderr,
                         "[seltrace] engine-call %s @ %s:%zu queue=%zu "
                         "callstack=%zu\n",
                         function.c_str(), script_name.c_str(), line,
                         tag_queue_.size(), call_stack_.size());
        }
        try {
            return lua_bridge().call_function(function, params);
        } catch (const oa::runtime::LuaError& e) {
            // research/130: [calllua] row (the boot chain: system_initlua /
            // system_loadinglua / system_dataloading / system_initialize /
            // system_starting). The error abandons only this row; the .iet
            // stream continues with the next instruction. Original runtime:
            // CallGlobal logs "calllua <fn>: <err>" and continues.
            lua_bridge().report_dispatch_error("calllua", function, e.what());
            return false;
        }
    };
}

oa::runtime::LuaBridge& Interpreter::lua_bridge() { return *lua_bridge_; }

uint64_t Interpreter::now_ms() const {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

const oa::runtime::Value* Interpreter::lookup_var_ptr(const std::string& name) const {
    const auto* map = variables_.domain_of(name);
    if (!map) return nullptr;
    const auto it = map->find(name);
    return it == map->end() ? nullptr : &it->second;
}

oa::runtime::LuaHost Interpreter::build_lua_host() {
    oa::runtime::LuaHost host;
    host.read_file = [this](const std::string& path) -> std::optional<std::vector<uint8_t>> {
        const std::string resolved = resolve_magic_path(path);
        if (hooks_.resource_read) return hooks_.resource_read(resolved);
        if (!hooks_.file_loader) return std::nullopt;
        return hooks_.file_loader(resolved);
    };
    host.is_file_exists = [this](const std::string& path) {
        const std::string resolved = resolve_magic_path(path);
        if (hooks_.resource_exists) return hooks_.resource_exists(resolved);
        if (!hooks_.file_loader) return false;
        return hooks_.file_loader(resolved).has_value();
    };
    host.get_var = [this](const std::string& name) { return lookup_var_ptr(name); };
    host.set_var = [this](const std::string& name, oa::runtime::Value v) {
        variables_.set(name, std::move(v));
    };
    host.enqueue_tag = [this](std::string tag, std::map<std::string, std::string> params) {
        if (tag == "reset" && std::getenv("OA_DEBUG_RESET")) {
            std::fprintf(stderr, "[reset] Lua e:tag reset queued from:\n%s",
                         lua_bridge().stack_trace(10).c_str());
        }
        if (tag == "trans" && std::getenv("OA_DEBUG_TRANSQ")) {
            std::string prm;
            for (const auto& [k, v] : params) prm += k + "=" + v + " ";
            std::fprintf(stderr, "[trq] Lua e:tag trans (%s) from:\n%s",
                         prm.c_str(), lua_bridge().stack_trace(12).c_str());
        }
        Instruction ins;
        ins.tag = std::move(tag);
        ins.params = std::move(params);
        ins.line = 0;
        tag_queue_.push_back(std::move(ins));
    };
    // Lua-originated e:tag{"calllua",...} runs synchronously,
    // mirroring the inline [calllua] tag of .asb streams (the [calllua]
    // branch of execute_tag in this TU) — not at the deferred queue drain.
    // The game UI framework dispatches per-button over=/out=/exec handlers
    // through e:tag{"calllua"...} and expects them to run while the
    // dispatching button group is still current: e.g. ui/dialog.lua
    // dialog_main calls btn_nonactive(btn.cursor) (which btn_out ->
    // e:tag calllua -> the slot's custom out handler) and only THEN
    // csvbtn3("dlg") swaps the group; with the deferred drain the queued
    // handler executed after the swap, getBtnInfo returned the empty table
    // and the game's custom
    // handler indexed nil (thyt exui.lua:100 user_saveqa_out ->
    // user_saveactive(nil) crash on every save-dialog open). Params are
    // resolved exactly like the asb path (function name + extra rows), so
    // Lua literals pass through untouched and ${..}/@ references behave
    // identically.
    host.run_calllua_sync = [this](const std::string& function,
                                   const std::map<std::string, std::string>& params) {
        if (function.empty() || !hooks_.call_lua_function) return false;
        const ExpressionEvaluator eval(variables_);
        const std::string resolved_fn = eval.resolve_param_str(function);
        if (resolved_fn.empty()) return true;  // optional callback -> silent
        std::map<std::string, std::string> extra;
        for (const auto& [k, v] : params) {
            if (k != "function") extra[k] = eval.resolve_param(v).to_string();
        }
        const std::string script =
            current_script() ? *current_script() : std::string();
        // Missing VM function = optional callback -> silent (call_function
        // returns false), identical to the asb calllua convention.
        (void)hooks_.call_lua_function(resolved_fn, extra, script, 0);
        return true;
    };
    host.apply_var_tag = [this](std::map<std::string, std::string> params) {
        apply_var(params);
        return true;
    };
    host.set_event_handler = [this](const std::string& ev, const std::string& fn) {
        if (fn.empty()) {
            event_handlers_.erase(ev);
        } else {
            event_handlers_[ev] = fn;
        }
    };
    host.call_handler = [this](const std::string& fn) {
        try {
            return lua_bridge().call_plain(fn);
        } catch (const oa::runtime::LuaError& e) {
            // research/130: engine-invoked handler (onSave/onLoad/... by name).
            lua_bridge().report_dispatch_error("handler", fn, e.what());
            return false;
        }
    };
    host.get_script_status = [] { return 0; };
    host.set_script_status = [](int) {};
    host.frame_number = [this] {
        if (hooks_.frame_number) return hooks_.frame_number();
        // No runtime behind (unit tests): project the interpreter clock.
        const uint64_t fps = config_.fps > 0 ? uint64_t(config_.fps) : 60;
        return now_ms() * fps / 1000;
    };
    host.get_wait_reason = [this] { return wait_reason_info_; };
    host.now_ms = [this] { return now_ms(); };
    host.platform = [this] { return variables_.platform; };
    host.set_magic_path = [this](const std::string& name, const std::string& path) {
        if (path.empty()) {
            magic_paths_.erase(name);
        } else {
            magic_paths_[name] = path;
        }
    };
    // e:loadPngComments — FPM 立绘/头像等把放置坐标写在 PNG tEXt 注释里
    // （image_fg.lua getfgfilepos: "pos,x,y"；mw face 的 fa 变体同源）。
    // 宿主回调 = 解析资源字节里的 tEXt 段（
    // parse_png_text_chunks 1018-1054；空注释 → nil）。openartemis 此前未
    // 接线（LuaHost 默认恒 nullopt）→ 立绘/头像部件坐标全部退化为 0。
    host.load_png_comments =
        [this](const std::string& path)
        -> std::optional<std::map<std::string, std::string>> {
        const std::string resolved = resolve_magic_path(path);
        std::optional<std::vector<uint8_t>> bytes;
        if (hooks_.resource_read) {
            bytes = hooks_.resource_read(resolved);
        } else if (hooks_.file_loader) {
            bytes = hooks_.file_loader(resolved);
        }
        if (!bytes) return std::nullopt;
        auto map = oa::media::png_text_chunks(*bytes);
        if (map.empty()) return std::nullopt;
        return map;
    };
    return host;
}

std::string Interpreter::resolve_magic_path(const std::string& name) const {
    // ":bg/rest" -> magic_paths_["bg"] + "rest"
    if (name.size() >= 2 && name[0] == ':') {
        const size_t slash = name.find('/');
        const std::string key = name.substr(1, slash == std::string::npos ? std::string::npos
                                                                         : slash - 1);
        const auto it = magic_paths_.find(key);
        if (it != magic_paths_.end()) {
            const std::string rest = slash == std::string::npos ? "" : name.substr(slash + 1);
            std::string base = it->second;
            if (!base.empty() && base.back() != '/') base.push_back('/');
            return base + rest;
        }
    }
    return name;
}

void Interpreter::seed_engine_variables_impl() {
    variables_.set("s.engineversion", Value::make_string("4.00"));
    variables_.set("s.windowsversion", Value::make_string("10.0"));
    variables_.set("s.screen_width", Value::make_int(config_.stage_width));
    variables_.set("s.screen_height", Value::make_int(config_.stage_height));
    variables_.set("s.savepath", Value::make_string("save"));
}

bool Interpreter::fire_event(const std::string& event_name) {
    const auto it = event_handlers_.find(event_name);
    if (it == event_handlers_.end() || it->second.empty()) return false;
    // Event handlers follow the FPM (e, p) convention (store/restore/
    // keyClickStart/exskip_end take the engine object as their first
    // argument, like calllua rows); call_function passes the engine handle
    // + an empty params table. Zero-arg handlers (vsync) ignore the extras.
    try {
        lua_bridge().call_function(it->second, {});
    } catch (const oa::runtime::LuaError& e) {
        // research/130: engine->script event (onEnterFrame=vsync, onSave=store,
        // ...). One failing event abandons the dispatch; the frame loop keeps
        // running (the original runtime's CallEvent logs and continues).
        lua_bridge().report_dispatch_error("event", it->second, e.what());
        return false;
    }
    return true;
}

void Interpreter::note_wait_reason(const Event& event) {
    wait_reason_info_.clear();
    if (event.kind != Event::Kind::Wait_) return;
    const WaitReason& r = event.reason;
    switch (r.kind) {
        case WaitReason::Kind::Timed:
            if (r.milliseconds > 0) {
                wait_reason_info_["time"] = std::to_string(now_ms() + r.milliseconds);
            }
            break;
        case WaitReason::Kind::ScenarioTween:
            wait_reason_info_[r.mode == 2 ? "textClearTween" : "textTween"] = "1";
            break;
        case WaitReason::Kind::Se:
            wait_reason_info_["sound"] = r.id;
            break;
        case WaitReason::Kind::VideoLayer:
            wait_reason_info_["video"] = r.id;
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// [var ...] application (simple + system= subset); shared by the var tag,
// Lua e:tag{"var",...} and Lua-side cond evaluation.
// ---------------------------------------------------------------------------

bool Interpreter::apply_var(const std::map<std::string, std::string>& params) {
    auto get = [&](const char* k) -> std::string {
        const auto it = params.find(k);
        return it == params.end() ? "" : it->second;
    };
    const ExpressionEvaluator eval(variables_);

    const auto sys = params.find("system");
    if (sys == params.end()) {
        // simple form: [var name=".." data=".."]
        variables_.set(get("name"), eval.resolve_param(get("data")));
        return true;
    }
    const std::string& system = sys->second;
    const std::string name = get("name");
    if (system == "os") {
        const std::string os =
            variables_.platform.empty() ? "unknown" : variables_.platform;
        variables_.set(name, Value::make_string(os));
        return true;
    }
    if (system == "delete") {
        // Delete requires an explicit variable name. A nameless [var
        // system=delete] targets nothing and must be a no-op, NOT a
        // clear-everything: several framework generations call it while
        // rebuilding UI sample text with only an `id=` layer param
        // (thyt system/ui/config.lua config_samplestart — every config
        // page draw, including the language-switch redraw). Wiping all
        // four domains there destroys the engine seeds (s.savepath,
        // s.screen_* …) plus the g.* pluto stashes mid-session, which
        // made every save-area existence check (isSaveFile / file checks
        // through e:var s.savepath) fail afterwards — load pages showed
        // empty slots and [load] could never fire.
        if (name.empty()) return true;
        auto erase_tree = [&](VariableStore::Map& m) {
            for (auto it = m.begin(); it != m.end();) {
                if (it->first == name || it->first.rfind(name + ".", 0) == 0) {
                    it = m.erase(it);
                } else {
                    ++it;
                }
            }
        };
        erase_tree(variables_.local);
        erase_tree(variables_.global);
        erase_tree(variables_.temp);
        erase_tree(variables_.system);
        return true;
    }
    if (system == "var_exist") {
        const std::string target = get("target");
        bool exists = false;
        if (get("local") == "1") {
            exists = variables_.local.count(target) > 0;
        } else {
            exists = variables_.get(target).has_value();
        }
        variables_.set(name, Value::make_bool(exists));
        return true;
    }
    if (system == "random") {
        const long long min_v = eval.resolve_param(get("min")).as_int().value_or(0);
        const long long max_v = eval.resolve_param(get("max")).as_int().value_or(0);
        if (max_v <= min_v) {
            variables_.set(name, Value::make_int(min_v));
        } else {
            const uint64_t seed = now_ms() ^ 0x9E3779B97F4A7C15ull;
            const uint64_t range = uint64_t(max_v - min_v + 1);
            const long long v = min_v + (long long)((seed * 6364136223846793005ull + 1) % range);
            variables_.set(name, Value::make_int(v));
        }
        return true;
    }
    if (system == "length") {
        const std::string src = eval.resolve_param_str(get("source"));
        const bool by_char = get("mode") == "1";
        if (by_char) {
            // count UTF-8 code points
            size_t count = 0;
            for (size_t i = 0; i < src.size();) {
                const unsigned char c = (unsigned char)src[i];
                i += (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 4;
                ++count;
            }
            variables_.set(name, Value::make_int((long long)count));
        } else {
            variables_.set(name, Value::make_int((long long)src.size()));
        }
        return true;
    }
    if (system == "unixtime") {
        variables_.set(name,
                       Value::make_int((long long)std::time(nullptr)));
        return true;
    }
    if (system == "date") {
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        variables_.set(name + ".year", Value::make_int(tm.tm_year + 1900));
        variables_.set(name + ".month", Value::make_int(tm.tm_mon + 1));
        variables_.set(name + ".day", Value::make_int(tm.tm_mday));
        variables_.set(name + ".hour", Value::make_int(tm.tm_hour));
        variables_.set(name + ".minute", Value::make_int(tm.tm_min));
        variables_.set(name + ".second", Value::make_int(tm.tm_sec));
        return true;
    }
    if (system == "file_exist" || system == "file_exists") {
        const std::string file = eval.resolve_param_str(get("file"));
        bool exists = false;
        if (get("save") == "1") {
            // save=1: 存档数据（保存区相对路径；  save
            // 分支，宿主按存档目录解析）。
            if (hooks_.save_file_exists) exists = hooks_.save_file_exists(file);
        } else if (hooks_.resource_exists) {
            exists = hooks_.resource_exists(resolve_magic_path(file));
        } else if (hooks_.file_loader) {
            exists = hooks_.file_loader(resolve_magic_path(file)).has_value();
        }
        variables_.set(name, Value::make_bool(exists));
        return true;
    }
    if (system == "file_update_time") {
        // 存档文件更新时刻（文档 var/file_update_time.md）：file 按保存区
        // 归一；format 缺省 yyyy/MM/dd hh:mm:ss；缺失落 noexist 原文。
        const std::string file = eval.resolve_param_str(get("file"));
        const std::string fmt = get("format");
        const std::string noexist = get("noexist");
        std::string value = noexist;
        if (hooks_.file_mtime) {
            if (auto c = hooks_.file_mtime(file)) {
                value = format_update_time(*c, fmt.empty() ? "yyyy/MM/dd hh:mm:ss" : fmt);
            }
        }
        variables_.set(name, Value::make_string(value));
        return true;
    }
    if (system == "get_sound_info") {
        // 声音播放状态（文档 var/get_sound_info.md；shape = 
        // apply_sound_info, ）。
        if (!hooks_.sound_info) {
            variables_.set(name, Value::make_int(0)); // 无宿主钩子保守回退
            return true;
        }
        const auto snap = hooks_.sound_info();
        if (!snap) {
            variables_.set(name, Value::make_int(0));
            return true;
        }
        const std::string id = eval.resolve_param_str(get("id"));
        if (!id.empty()) {
            // 有 id：该 SE 的 playing/gain/pan；不存在仅 name.playing=0。
            const auto it = std::find_if(snap->se.begin(), snap->se.end(),
                                         [&](const auto& c) { return c.id == id; });
            if (it != snap->se.end()) {
                variables_.set(name + ".playing", Value::make_int(it->playing ? 1 : 0));
                variables_.set(name + ".gain", Value::make_int(it->gain));
                variables_.set(name + ".pan", Value::make_int(it->pan));
            } else {
                variables_.set(name + ".playing", Value::make_int(0));
            }
            return true;
        }
        const int64_t playing = snap->bgm && snap->bgm->playing ? 1 : 0;
        const int64_t gain = snap->bgm ? snap->bgm->gain : 1000;
        const int64_t pan = snap->bgm ? snap->bgm->pan : 0;
        variables_.set(name + ".playing", Value::make_int(playing));
        variables_.set(name + ".gain", Value::make_int(gain));
        variables_.set(name + ".pan", Value::make_int(pan));
        size_t index = 0;
        for (const auto& ch : snap->se) {
            variables_.set(name + "." + std::to_string(index) + ".id",
                           Value::make_string(ch.id));
            variables_.set(name + "." + std::to_string(index) + ".playing",
                           Value::make_int(ch.playing ? 1 : 0));
            variables_.set(name + "." + std::to_string(index) + ".gain",
                           Value::make_int(ch.gain));
            variables_.set(name + "." + std::to_string(index) + ".pan",
                           Value::make_int(ch.pan));
            ++index;
        }
        variables_.set(name + ".size", Value::make_int(int64_t(snap->se.size())));
        return true;
    }
    if (system == "screen_width" || system == "screen_height") {
        variables_.set(name, Value::make_int(system == "screen_width" ? config_.stage_width
                                                                       : config_.stage_height));
        return true;
    }
    if (system == "get_layer_info") {
        // Slider readback (slider_dragX/xslider_pin …): dump every layer
        // prop into name.<prop> so Lua can read name.left/name.top/etc. No
        // layer -> nothing is written (script checks e:var existence).
        // (snll sample preview on text-page entry): Lua e:tag
        // events are QUEUED (enqueue_tag) while this var tag applies
        // synchronously, so a read following e:tag{'lyc2'...} in the SAME
        // Lua call used to see the PRE-lyc2 scene (the queried layer missing
        // -> empty dump -> snll config_samplestart's alpha==255 guard failed
        // on first page-2 entry and the sample round never armed). Ordered
        // Ordered
        // semantics: a sync var evaluates at its position in
        // the tag stream — every tag queued before it has already run — so
        // run that prefix before reading the layer.
        flush_pending_text_tags();
        const std::string id = eval.resolve_param_str(get("id"));
        if (hooks_.layer_info && !id.empty()) {
            const auto props = hooks_.layer_info(id);
            for (const auto& [k, v] : props) variables_.set(name + "." + k, Value::make_string(v));
        }
        return true;
    }
    // misc-A: backlog 查询桥（ HostQueryHooks，）。参
    // 参数语义：name/page/id/allfont 均按原文取值（不解析表达式——
    // apply_var_tag 的表达式参数表不含 id/page/name）；allfont 为 tag_bool 开。
    if (system == "get_backlog_size") {
        // name = 总历史页数；无钩子 → 保守回退 0。
        const size_t size = hooks_.backlog_size ? hooks_.backlog_size() : 0;
        variables_.set(name, Value::make_int((long long)size));
        return true;
    }
    if (system == "get_backlog_tags") {
        long long page = 0;
        try {
            page = std::stoll(get("page"));
        } catch (...) {
            page = 0;
        }
        if (page < 0) page = 0;
        const bool allfont = tag_bool(get("allfont"), false);
        std::optional<std::vector<std::string>> tags =
            hooks_.backlog_tags ? hooks_.backlog_tags((size_t)page, allfont) : std::nullopt;
        // 伪数组 name.0..N-1 + name.size；未命中/越界 → 只落 size=0。
        if (tags) {
            for (size_t i = 0; i < tags->size(); ++i) {
                variables_.set(name + "." + std::to_string(i),
                               Value::make_string((*tags)[i]));
            }
            variables_.set(name + ".size", Value::make_int((long long)tags->size()));
        } else {
            variables_.set(name + ".size", Value::make_int(0));
        }
        return true;
    }
    if (system == "get_message_tags") {
        const std::string id = get("id");
        const bool allfont = tag_bool(get("allfont"), false);
        std::optional<std::vector<std::string>> tags =
            hooks_.message_tags ? hooks_.message_tags(id, allfont) : std::nullopt;
        if (tags) {
            for (size_t i = 0; i < tags->size(); ++i) {
                variables_.set(name + "." + std::to_string(i),
                               Value::make_string((*tags)[i]));
            }
            variables_.set(name + ".size", Value::make_int((long long)tags->size()));
        } else {
            variables_.set(name + ".size", Value::make_int(0));
        }
        return true;
    }
    // 当前消息层文本度量（ message_layer_metrics）。
    // Lua get_fontsize 创建临时消息层、排入测量文本后通过这组 key 读取排版尺寸，
    // 用于字体替换时的等比缩放。无钩子 → 0（Lua 侧 math.ceil(35*0/0) = nan →
    // 字体参数不生效；因此必须接入宿主度量）。
    if (system == "get_message_layer_width" ||
        system == "get_message_layer_height" ||
        system == "get_message_layer_line_width") {
        // FPM writes these queries right after queueing a
        // chgmsg/rp/print segment (uihelp centering ui.lua:89-92, get_fontsize
        // boot.lua:334-345, line/backlog measuring) — the segment must take
        // effect first or the sync query reads the previous active layer.
        if (std::getenv("OA_DEBUG_VAR")) {
            std::fprintf(stderr, "[var] queue before flush:");
            for (size_t qi = 0; qi < tag_queue_.size() && qi < 14; ++qi) {
                const Instruction& q = tag_queue_[qi];
                const auto idit = q.params.find("id");
                std::fprintf(stderr, " <%s%s%s>", q.tag.c_str(),
                             idit != q.params.end() ? " id=" : "",
                             idit != q.params.end() ? idit->second.c_str() : "");
            }
            std::fprintf(stderr, " total=%zu\n", tag_queue_.size());
        }
        flush_pending_text_tags();
        double width = 0, height = 0, line_width = 0;
        if (hooks_.message_layer_metrics) {
            const auto [w, h, lw] = hooks_.message_layer_metrics();
            width = w; height = h; line_width = lw;
        }
        const double value = (system == "get_message_layer_width") ? width
                           : (system == "get_message_layer_height") ? height
                           : line_width;
        variables_.set(name, Value::make_string(std::to_string(value)));
        return true;
    }
    // Remaining var system= keys (substr/explode/find/...) are added as their
    // host services land; silently ignored for now.
    return true;
}

// ---------------------------------------------------------------------------
// Queued-tag drain (flushes the pending tag queue)
// ---------------------------------------------------------------------------

std::optional<ExecutionResult> Interpreter::flush_tag_queue() {
    while (!tag_queue_.empty()) {
        Instruction ins = std::move(tag_queue_.front());
        tag_queue_.erase(tag_queue_.begin());
        if (on_step) on_step(*current_script_name_, current_line_, ins);

        if (std::getenv("OA_SELTRACE") && ins.tag == "call" &&
            current_script_name_ && *current_script_name_ == "system/script.asb" &&
            current_line_ <= 2) {
            const std::string* lf = ins.get("label");
            const std::string* ff = ins.get("file");
            std::fprintf(stderr,
                         "[seltrace] QCALL@LOW label=%s file=%s pos=%s:%zu q=%zu\n",
                         lf ? lf->c_str() : "-", ff ? ff->c_str() : "-",
                         current_script_name_->c_str(), current_line_,
                         tag_queue_.size());
        }
        const TagOutcome outcome = execute_tag(*this, ins, false);
        switch (outcome.kind) {
            case TagOutcomeKind::Continue:
                continue;
            case TagOutcomeKind::Jump:
                queued_saw_jump_ = true;
                arrived_by_jump_ = true;
                current_line_ = outcome.line;
                continue;
            case TagOutcomeKind::JumpExternal: {
                queued_saw_jump_ = true;
                load_external_script(outcome.file);
                const Script* t = get_script(outcome.file);
                const auto line = t->get_label_line(outcome.label);
                if (!line) {
                    throw ScriptError("label", "jump: label not found: " + outcome.label, 0);
                }
                current_script_name_ = outcome.file;
                current_line_ = *line;
                arrived_by_jump_ = true;
                continue;
            }
            case TagOutcomeKind::Call: {
                queued_saw_call_ = true;
                // queued call: return_line = current_line (do NOT add 1)
                const size_t pinned = call_stack_.size();
                call_stack_.push_back(CallFrame{*current_script_name_, current_line_});
                if (std::getenv("OA_SELTRACE")) {
                    const std::string* lf = ins.get("label");
                    const std::string* ff = ins.get("file");
                    std::fprintf(stderr,
                                 "[seltrace] QCALL label=%s file=%s pos=%s:%zu "
                                 "line=%zu depth=%zu\n",
                                 lf ? lf->c_str() : "-", ff ? ff->c_str() : "-",
                                 current_script_name_ ? current_script_name_->c_str()
                                                      : "?",
                                 current_line_, current_line_, call_stack_.size());
                    // full stack at deep pushes — the
                    // {script.asb:0} resurrection frame's push was invisible
                    // to the shallow QCALL line prints.
                    if (call_stack_.size() >= 5) {
                        std::string cst;
                        for (const auto& fr : call_stack_) {
                            cst += fr.script + ":" +
                                   std::to_string(fr.return_line) + " ";
                        }
                        std::fprintf(stderr, "[seltrace]   QCALL-STACK [%s]\n",
                                     cst.c_str());
                    }
                }
                (void)pinned;
                const std::string target = outcome.file.empty() ? *current_script_name_
                                                                : outcome.file;
                if (!outcome.file.empty()) load_external_script(target);
                const Script* t = get_script(target);
                const auto line = t->get_label_line(outcome.label);
                if (!line) {
                    throw ScriptError("label", "call: label not found: " + outcome.label, 0);
                }
                current_script_name_ = target;
                current_line_ = *line;
                arrived_by_jump_ = true;
                continue;
            }
            case TagOutcomeKind::Return: {
                if (!call_stack_.empty()) {
                    const std::string origin_script =
                        current_script_name_ ? *current_script_name_ : "";
                    const size_t origin_line = current_line_;
                    const CallFrame frame = call_stack_.back();
                    call_stack_.pop_back();
                    current_script_name_ = frame.script;
                    current_line_ = frame.return_line;
                    // resurrection probe: a [return]
                    // landing on the game's select-[stop] area
                    // (system/script.asb line <= 1) re-parks a consumed
                    // choice park (482 crash family). Print the return's
                    // origin and the popped frame.
                    if (std::getenv("OA_SELTRACE") &&
                        current_script_name_ &&
                        *current_script_name_ == "system/script.asb" &&
                        current_line_ <= 1) {
                        std::string cst;
                        for (const auto& fr : call_stack_) {
                            cst += fr.script + ":" + std::to_string(fr.return_line) + " ";
                        }
                        std::fprintf(stderr,
                                     "[seltrace] RET->selectarea origin=%s:%zu "
                                     "popped=%s:%zu stack[%s]\n",
                                     origin_script.c_str(), origin_line,
                                     frame.script.c_str(), frame.return_line,
                                     cst.c_str());
                    }
                    continue;
                }
                return ExecutionResult::Completed;
            }
            case TagOutcomeKind::WaitEvent:
            case TagOutcomeKind::CustomEvent: {
                const CallbackResult cr = callback_(outcome.event);
                if (cr == CallbackResult::Continue) continue;
                if (cr == CallbackResult::Pause) {
                    last_wait_from_queue_ = true;
                    note_wait_reason(outcome.event);
                    last_wait_event_ = outcome.event;
                    return ExecutionResult::Wait;
                }
                throw ScriptError("aborted", "callback aborted", 0);
            }
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Message-text segment tags a sync metric var may pre-execute.
// chgmsg/rp/print/rt/ruby/font/scetween/... mutate the message-layer state
// the metric query reads; waits and control tags never run here (they are
// not part of a measuring segment), so the interpreter wait machinery is
// untouched.
// ---------------------------------------------------------------------------
namespace {
// tags a sync metric var must NOT flush past — in ordered (real-engine)
// execution these pause/redirect the stream or re-enter Lua, so the var
// behind them would not have been reached yet.
bool is_var_barrier_stop_tag(const std::string& tag) {
    static const char* kStop[] = {
        "wait",    "call",    "jump",    "return",  "calllua",
        "exlabel", "label",
        "macroadd","exkey",   "estag",   "/if",     "loop",
        "/loop",   "exit",    "gotitle", "yesno",
    };
    for (const char* t : kStop)
        if (tag == t) return true;
    return false;
}
} // namespace

void Interpreter::flush_pending_text_tags() {
    // A sync metric var evaluates at ITS position in the ordered tag
    // stream of the current Lua event (FPM semantics) — every tag queued
    // before it has already run, so the metric reads the layer/page state
    // the chain built. The flush pre-executes that prefix IN ORDER (text
    // tags first; the same rule extends to benign scene/presentation
    // tags such as flip/lydel/lyc2/lytween queued between text segments —
    // e.g. the previous button's btn_out 'flip' when the pointer hops
    // straight from one button to another, or the backlog page rebuild's
    // lydel before re-creating row layers). Skipping such tags instead of
    // executing them was wrong: the later text tags would run against the
    // PRE-lydel state and the deferred lydel would then delete the freshly
    // rebuilt layers (backlog rows lost their scene nodes under e8411fa).
    // The flush still stops at tags that pause/redirect the stream or
    // re-enter Lua (the var would not have been reached in ordered
    // execution), and at any tag whose execution yields such an outcome.
    size_t ran = 0;
    for (size_t i = 0; i < tag_queue_.size();) {
        const Instruction& front = tag_queue_[i];
        const std::string& t = front.tag;
        if (std::getenv("OA_DEBUG_VAR")) {
            const auto idit = front.params.find("id");
            std::fprintf(stderr, "[var] barrier head tag='%s'%s%s\n",
                         t.c_str(), idit != front.params.end() ? " id=" : "",
                         idit != front.params.end() ? idit->second.c_str() : "");
        }
        if (is_var_barrier_stop_tag(t)) {
            if (std::getenv("OA_DEBUG_VAR"))
                std::fprintf(stderr,
                             "[var] barrier STOP at '%s' remains=%zu\n",
                             t.c_str(), tag_queue_.size() - i);
            break;
        }
        Instruction ins = std::move(tag_queue_[i]);
        tag_queue_.erase(tag_queue_.begin() + i);
        if (on_step) on_step(*current_script_name_, current_line_, ins);
        const TagOutcome outcome = execute_tag(*this, ins, false);
        ++ran;
        switch (outcome.kind) {
            case TagOutcomeKind::Continue:
                break;
            case TagOutcomeKind::Jump:
            case TagOutcomeKind::JumpExternal:
            case TagOutcomeKind::Call:
            case TagOutcomeKind::Return:
                // Stream redirected: the var behind it would not have been
                // reached — leave the rest to the normal drain.
                if (std::getenv("OA_DEBUG_VAR"))
                    std::fprintf(stderr,
                                 "[var] barrier redirect at '%s' remains=%zu\n",
                                 ins.tag.c_str(), tag_queue_.size() - i);
                i = tag_queue_.size();
                break;
            case TagOutcomeKind::WaitEvent:
            case TagOutcomeKind::CustomEvent: {
                // A queued tag paused the runtime — the var behind it would
                // not have been reached; let the normal drain own the wait.
                const CallbackResult cr = callback_(outcome.event);
                if (cr != CallbackResult::Continue) i = tag_queue_.size();
                break;
            }
        }
        if (i >= tag_queue_.size()) break;
        // Erasing shifts the tail; re-examine the same index.
    }
    if (std::getenv("OA_DEBUG_VAR"))
        std::fprintf(stderr, "[var] barrier ran=%zu\n", ran);
}

} // namespace oa::runtime
