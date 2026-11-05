#include "core/render/text.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace oa::render {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static std::string_view trimv(std::string_view v) {
    while (!v.empty() && std::isspace((unsigned char)v.front())) v.remove_prefix(1);
    while (!v.empty() && std::isspace((unsigned char)v.back())) v.remove_suffix(1);
    return v;
}

static bool parse_u64s(std::string_view v, uint64_t* out) {
    v = trimv(v);
    if (v.empty()) return false;
    const std::string s(v);
    char* end = nullptr;
    errno = 0;
    const double d = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || errno == ERANGE || !std::isfinite(d)) return false;
    if (d <= 0.0) {
        *out = 0;
        return true;
    }
    if (d >= 1e12) {
        *out = 1000000000000ull;
        return true;
    }
    *out = uint64_t(std::llround(d));
    return true;
}
static bool parse_dbl(std::string_view v, double* out) {
    v = trimv(v);
    if (v.empty()) return false;
    const std::string s(v);
    char* end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(d)) return false;
    *out = d;
    return true;
}

static std::string escape_attr(const std::string& v) {
    std::string r;
    r.reserve(v.size());
    for (char c : v) {
        if (c == '\\') r += "\\\\";
        else if (c == '"') r += "\\\"";
        else r += c;
    }
    return r;
}

// FontDesc -------------------------------------------------------------
const std::string* FontDesc::get(std::string_view key) const {
    const auto it = raw.find(std::string(key));
    return it == raw.end() ? nullptr : &it->second;
}
std::string FontDesc::get_or(std::string_view key, const std::string& fallback) const {
    const std::string* v = get(key);
    return v ? *v : fallback;
}
double FontDesc::size() const {
    const std::string* s = get("size");
    if (!s) return kDefaultFontSize;
    double d = 0;
    if (!parse_dbl(*s, &d)) return kDefaultFontSize;
    return d;
}
std::string FontDesc::face() const { return get_or("face", ""); }
std::string FontDesc::color() const { return get_or("color", ""); }
double FontDesc::spacetop() const {
    const std::string* s = get("spacetop");
    double d = 0;
    if (s && parse_dbl(*s, &d)) return d;
    return 0.0;
}
double FontDesc::spacemiddle() const {
    const std::string* s = get("spacemiddle");
    double d = 0;
    if (s && parse_dbl(*s, &d)) return d;
    return 0.0;
}
double FontDesc::spacebottom() const {
    const std::string* s = get("spacebottom");
    double d = 0;
    if (s && parse_dbl(*s, &d)) return d;
    return 0.0;
}
double FontDesc::rubysize() const {
    const std::string* s = get("rubysize");
    double d = 0;
    if (s && parse_dbl(*s, &d)) return d;
    return 0.0;
}
void FontDesc::merge_raw(const std::map<std::string, std::string>& p) {
    for (const auto& [k, v] : p) raw[k] = v; // 参数保形
}

// BacklogTag / BacklogPage ---------------------------------------------
static std::string build_tag(const std::string& name,
                                   const std::map<std::string, std::string>& params) {
    std::vector<std::string> keys;
    for (const auto& [k, v] : params) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    std::string s = "[" + name;
    for (const std::string& k : keys) {
        s += " " + k + "=\"" + escape_attr(params.at(k)) + "\"";
    }
    s += "]";
    return s;
}

std::string BacklogTag::get_string() const {
    switch (kind) {
        case Kind::Text:
            return "[print data=\"" + escape_attr(text) + "\"]";
        case Kind::LineBreak:
            return "[rt]";
        case Kind::Font:
            return build_tag("font", font);
        case Kind::RubyStart:
            return "[ruby text=\"" + escape_attr(text) + "\"]";
        case Kind::RubyEnd:
            return "[/ruby]";
    }
    return "";
}

bool BacklogPage::has_text() const {
    for (const BacklogTag& t : tags)
        if (t.kind == BacklogTag::Kind::Text) return true;
    return false;
}
std::vector<std::string> BacklogPage::reproduction_tags(bool allfont) const {
    std::vector<std::string> out;
    if (allfont && !page_font.empty()) {
        out.push_back(build_tag("font", page_font));
    }
    for (const BacklogTag& t : tags) out.push_back(t.get_string());
    return out;
}

// GlyphIconConfig -------------------------------------------------------
GlyphIconConfig glyph_icon_from_raw(const std::map<std::string, std::string>& raw) {
    GlyphIconConfig c;
    auto non_empty = [&](const char* k) -> const std::string* {
        const auto it = raw.find(k);
        if (it == raw.end()) return nullptr;
        const std::string& v = it->second;
        const std::string_view t = trimv(v);
        return t.empty() ? nullptr : &it->second;
    };
    auto set_d = [&](const char* k, double* d) {
        const std::string* v = non_empty(k);
        if (v) (void)parse_dbl(*v, d);
    };
    if (const std::string* v = non_empty("layer")) c.layer = *v;
    if (const std::string* v = non_empty("rplayer")) c.rplayer = *v;
    set_d("left", &c.left);
    set_d("top", &c.top);
    set_d("rpleft", &c.rpleft);
    set_d("rptop", &c.rptop);
    if (const std::string* v = non_empty("homing")) c.homing = *v == "1";
    return c;
}

// default layout charsets
static const char* kDefaultProhibitHead = "!?%)]},.:;、。，．・：；！？」』）｝〕］】";
static const char* kDefaultProhibitFoot = "([{「『（｛〔［【";
static const char* kDefaultWordparts =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

// 单帧最多推进的逐字时间（长帧不折算成批量出字）。
static constexpr uint64_t kRevealMaxStepMs = 50;

// UTF-8 iteration --------------------------------------------------------
bool next_codepoint(std::string_view s, size_t* i, uint32_t* cp) {
    if (*i >= s.size()) return false;
    const uint8_t c = (uint8_t)s[*i];
    if (c < 0x80) {
        *cp = c;
        ++*i;
        return true;
    }
    int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
    uint32_t v = c & (extra == 1 ? 0x1F : extra == 2 ? 0x0F : 0x07);
    for (int k = 1; k <= extra; ++k) {
        if (*i + (size_t)k >= s.size()) return false;
        v = (v << 6) | ((uint8_t)s[*i + (size_t)k] & 0x3F);
    }
    *i += (size_t)extra + 1;
    *cp = v;
    return true;
}

static bool contains_cp(std::string_view set, uint32_t cp) {
    size_t i = 0;
    uint32_t c = 0;
    while (next_codepoint(set, &i, &c)) {
        if (c == cp) return true;
    }
    return false;
}

// TextEngine -------------------------------------------------------------
TextEngine::TextEngine() {
    layout_cfg_.prohibit_head = kDefaultProhibitHead;
    layout_cfg_.prohibit_foot = kDefaultProhibitFoot;
    layout_cfg_.wordparts = kDefaultWordparts;
    default_font_.raw["face"] = std::string(kDefaultFontFile);
}

void TextEngine::bump_revision() { ++revision_; }

void TextEngine::clear_scene() {
    active_.reset();
    layer_stack_.clear();
    ruby_open_ = false;
    ruby_annotation_.clear();
    // 按 id 消息层的字体/几何配置 = 会话级:
    for (auto& [id, l] : layers_) {
        (void)id;
        l.page.clear();
        l.page_tags.clear();
        l.page_font = l.font.raw; // 页首快照重置为当前字体
        l.reveal_index = 0;
        l.reveal_pending = false;
        l.reveal_clock_ms = 0;
        l.reveal_fresh = false;
        l.char_count = 0;
        l.text_hidden = false;
        ++l.generation;
    }
    bump_revision();
}

static bool is_blank_cp(uint32_t cp);

void TextEngine::anchor_active_to_content() {
    // 只认"含非空白字形、未隐藏"的内容层；空白测量层（basefont.font0x
    if (!layer_stack_.empty()) return;
    std::string best;
    size_t best_chars = 0;
    for (const auto& [id, l] : layers_) {
        if (l.text_hidden || l.page.empty() || l.char_count == 0) continue;
        bool has_visible = false;
        for (const PageUnit& u : l.page) {
            if (u.kind == PageUnit::Kind::Newline || u.data.empty()) continue;
            size_t i = 0;
            uint32_t cp = 0;
            while (next_codepoint(u.data, &i, &cp)) {
                if (!is_blank_cp(cp)) {
                    has_visible = true;
                    break;
                }
            }
            if (has_visible) break;
        }
        if (!has_visible) continue;
        if (l.char_count > best_chars) {
            best_chars = l.char_count;
            best = id;
        }
    }
    if (best.empty()) return;
    active_ = best;
    layer_stack_.clear();
    bump_revision();
}

std::string TextEngine::anon_id() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const uint64_t nanos =
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    char buf[72];
    std::snprintf(buf, sizeof(buf), "chgmsg_%llx_%llu", (unsigned long long)nanos,
                  (unsigned long long)anonymous_serial_++);
    return std::string(buf);
}

size_t TextEngine::recompute_char_count(MessageLayer& l) const {
    size_t n = 0;
    for (const PageUnit& u : l.page) {
        if (u.kind == PageUnit::Kind::Newline) {
            ++n; // 换行字形也计入缓冲长度（reveal_index 语义）
            continue;
        }
        size_t i = 0;
        uint32_t cp = 0;
        while (next_codepoint(u.data, &i, &cp)) ++n;
    }
    l.char_count = n;
    return n;
}

MessageLayer& TextEngine::active_mut() {
    const std::string id = active_.value_or(std::string(kDefaultMessageLayer));
    active_ = id;
    auto it = layers_.find(id);
    if (it == layers_.end()) {
        MessageLayer l;
        l.id = id;
        l.font = default_font_;
        l.page_font = default_font_.raw;
        auto res = layers_.emplace(id, std::move(l));
        it = res.first;
    }
    return it->second;
}

std::string TextEngine::active_layer_id() const {
    return active_.value_or(std::string(kDefaultMessageLayer));
}
const MessageLayer* TextEngine::layer(std::string_view id) const {
    const auto it = layers_.find(std::string(id));
    return it == layers_.end() ? nullptr : &it->second;
}

void TextEngine::switch_layer(std::optional<std::string> id, bool stack,
                              std::optional<int> layered) {
    if (stack && active_) {
        layer_stack_.push_back(*active_);
    }
    if (!id || id->empty()) id = anon_id();
    active_ = *id;
    MessageLayer& l = active_mut();
    // layered only changes when the tag explicitly carries it — a
    if (layered.has_value()) l.layered = *layered == 1;

    // NO cross-layer geometry/font inheritance. A message
    bump_revision();
}

void TextEngine::pop_layer() {
    if (!layer_stack_.empty()) {
        active_ = layer_stack_.back();
        layer_stack_.pop_back();
    }
    bump_revision();
}

void TextEngine::apply_font(const std::map<std::string, std::string>& p) {
    MessageLayer& l = active_mut();
    // entirexscale / entireyscale: the framework's font rows are authored in an
    std::map<std::string, std::string> scaled = p;
    double sx = 100.0;
    double sy = 100.0;
    if (const auto it = p.find("entirexscale"); it != p.end()) parse_dbl(it->second, &sx);
    if (const auto it = p.find("entireyscale"); it != p.end()) parse_dbl(it->second, &sy);
    if (sx <= 0.0) sx = 100.0;
    if (sy <= 0.0) sy = 100.0;
    if (sx != 100.0 || sy != 100.0) {
        auto scale_key = [&](const char* key, double factor) {
            const auto it = scaled.find(key);
            if (it == scaled.end()) return;
            double v = 0.0;
            if (!parse_dbl(it->second, &v)) return;
            it->second = std::to_string(v * factor / 100.0);
        };
        scale_key("size", sy);
        scale_key("kerning", sx);
        // width/height describe the TEXT BOX in the enlarged font space (the
        scale_key("width", sx);
        scale_key("height", sy);
        scale_key("spacetop", sy);
        scale_key("spacemiddle", sy);
        scale_key("spacebottom", sy);
        scale_key("outline", sy);
        scale_key("rubysize", sy);
        scaled["entirexscale"] = "100";
        scaled["entireyscale"] = "100";
    }
    const std::map<std::string, std::string>& p_use = scaled;
    const auto sit = p_use.find("stack");
    const bool stacked = sit == p_use.end() || sit->second == "1" || sit->second == "true";
    if (stacked) l.font_stack.push_back(l.font);
    l.font.merge_raw(p_use);
    auto set_d = [&p_use](const char* key, double* dst) {
        const auto it = p_use.find(key);
        if (it == p_use.end()) return;
        (void)parse_dbl(it->second, dst);
    };
    set_d("left", &l.left);
    set_d("top", &l.top);
    set_d("width", &l.width);
    set_d("height", &l.height);
    // 排版键 → 本层获得绘制盒（无盒层只能经脚本显式节点绘制,孤儿打印
    // 不绘制;继承/默认字体不置位,见 MessageLayer::positioned）。
    if (p_use.count("left") || p_use.count("top") || p_use.count("width") ||
        p_use.count("height"))
        l.positioned = true;
    // 再现记录：font 参数原样入本页标签序列（get_message_tags/backlog）
    BacklogTag t;
    t.kind = BacklogTag::Kind::Font;
    t.font = p_use;
    l.page_tags.push_back(std::move(t));
    bump_revision();
}

void TextEngine::font_init() {
    MessageLayer& l = active_mut();
    l.font = default_font_;
    l.font_stack.clear();
    BacklogTag t;
    t.kind = BacklogTag::Kind::Font;
    t.font = l.font.raw;
    l.page_tags.push_back(std::move(t));
    bump_revision();
}

void TextEngine::font_close() {
    MessageLayer& l = active_mut();
    if (!l.font_stack.empty()) {
        l.font = l.font_stack.back();
        l.font_stack.pop_back();
        BacklogTag t;
        t.kind = BacklogTag::Kind::Font;
        t.font = l.font.raw;
        l.page_tags.push_back(std::move(t));
        bump_revision();
    }
}

void TextEngine::font_default(const std::map<std::string, std::string>& p) {
    default_font_.merge_raw(p);
}

void TextEngine::push_text(const std::string& content) {
    MessageLayer& l = active_mut();
    // 新页首内容 → 进入逐字揭示状态（push_text was_empty 检查）
    if (recompute_char_count(l) == 0) {
        l.reveal_pending = true;
        l.reveal_clock_ms = 0;
        l.reveal_index = 0;
        l.reveal_fresh = true;
    }
    // 内容内换行（LF）语义归一。push_text 把 content 按 '\n' 切成
    auto push_segment = [&](const std::string& data) {
        PageUnit u;
        u.kind = ruby_open_ ? PageUnit::Kind::RubyBase : PageUnit::Kind::Text;
        u.data = data;
        if (ruby_open_) u.ruby_annotation = ruby_annotation_;
        u.font = l.font;
        l.page.push_back(std::move(u));
        BacklogTag t;
        t.kind = BacklogTag::Kind::Text;
        t.text = data;
        l.page_tags.push_back(std::move(t));
    };
    if (content.empty()) {
        push_segment(content);
        recompute_char_count(l);
        bump_revision();
        return;
    }
    size_t pos = 0;
    for (;;) {
        const size_t nl = content.find('\n', pos);
        if (nl == std::string::npos) {
            if (pos < content.size()) push_segment(content.substr(pos)); // 尾段(空则跳过)
            break;
        }
        size_t end = nl;
        if (end > pos && content[end - 1] == '\r') --end; // CRLF: '\r' 丢弃
        if (end > pos) push_segment(content.substr(pos, end - pos));
        PageUnit nu;
        nu.kind = PageUnit::Kind::Newline;
        nu.font = l.font;
        l.page.push_back(std::move(nu));
        BacklogTag nt;
        nt.kind = BacklogTag::Kind::LineBreak;
        l.page_tags.push_back(std::move(nt));
        pos = nl + 1;
    }
    recompute_char_count(l);
    bump_revision();
}

void TextEngine::line_break(bool omit_blank_line) {
    MessageLayer& l = active_mut();
    const bool page_empty = l.page.empty();
    const bool last_is_newline = !l.page.empty() && l.page.back().kind == PageUnit::Kind::Newline;
    if (omit_blank_line && (page_empty || last_is_newline)) return;
    PageUnit u;
    u.kind = PageUnit::Kind::Newline;
    u.font = l.font;
    l.page.push_back(std::move(u));
    BacklogTag t;
    t.kind = BacklogTag::Kind::LineBreak;
    l.page_tags.push_back(std::move(t));
    bump_revision();
}

void TextEngine::page_break(std::optional<int> backlog_param) {
    MessageLayer& l = active_mut();
    // rp 参数 → 入库：1/0 无视 writebacklog；缺省按 write_mode
    auto should_store = [&]() -> bool {
        if (backlog_param.has_value()) return backlog_param.value() == 1;
        return backlog_write_mode_;
    };
    if (should_store()) {
        BacklogPage page;
        page.page_font = l.page_font;
        page.tags = l.page_tags;
        if (!backlog_allow_ || !page.has_text()) {
            // 不入库
        } else {
            if (!backlog_include_font_) {
                page.page_font.clear();
                page.tags.erase(
                    std::remove_if(page.tags.begin(), page.tags.end(),
                                   [](const BacklogTag& t) {
                                       return t.kind == BacklogTag::Kind::Font;
                                   }),
                    page.tags.end());
            }
            backlog_pages_.push_back(std::move(page));
            while (backlog_pages_.size() > kBacklogMaxPages) {
                backlog_pages_.erase(backlog_pages_.begin());
            }
        }
    }
    l.page.clear();
    l.page_tags.clear();
    l.page_font = l.font.raw; // 以当前字体重置页首快照
    l.generation++;
    l.reveal_index = 0;
    l.reveal_pending = false;
    l.reveal_clock_ms = 0;
    l.reveal_fresh = false;
    l.char_count = 0;
    ruby_open_ = false;
    bump_revision();
}

void TextEngine::ruby_start(const std::string& annotation) {
    ruby_open_ = true;
    ruby_annotation_ = annotation;
    BacklogTag t;
    t.kind = BacklogTag::Kind::RubyStart;
    t.text = annotation;
    MessageLayer& l = active_mut();
    l.page_tags.push_back(std::move(t));
    bump_revision();
}
void TextEngine::ruby_end() {
    ruby_open_ = false;
    ruby_annotation_.clear();
    BacklogTag t;
    t.kind = BacklogTag::Kind::RubyEnd;
    MessageLayer& l = active_mut();
    l.page_tags.push_back(std::move(t));
    bump_revision();
}

void TextEngine::set_text_hidden(bool hidden) {
    active_mut().text_hidden = hidden;
    bump_revision();
}

void TextEngine::set_prohibit(const std::string& head, const std::string& foot) {
    if (!head.empty()) layout_cfg_.prohibit_head = head;
    if (!foot.empty()) layout_cfg_.prohibit_foot = foot;
}
void TextEngine::set_wordparts(const std::string& parts) { layout_cfg_.wordparts = parts; }
void TextEngine::set_indent(const std::string& pair) { layout_cfg_.indent_pair = pair; }

// [scetween]：type/mode/param/ease/diff/delay/time/randomdelay
// （ScetweenConfig::from_params）
void TextEngine::apply_scetween(const std::map<std::string, std::string>& p) {
    auto get = [&p](const char* k) -> std::string {
        const auto it = p.find(k);
        return it == p.end() ? std::string() : it->second;
    };
    ScetweenConfig c;
    const std::string type = get("type");
    c.mode = (type == "out") ? ScetweenMode::Out
              : (type == "show") ? ScetweenMode::Show
              : (type == "hide") ? ScetweenMode::Hide
              : (type == "backlog_down_in") ? ScetweenMode::BacklogDownIn
              : (type == "backlog_down_out") ? ScetweenMode::BacklogDownOut
              : (type == "backlog_up_in") ? ScetweenMode::BacklogUpIn
              : (type == "backlog_up_out") ? ScetweenMode::BacklogUpOut
                                          : ScetweenMode::In;
    c.set_add = get("mode") == "add";
    c.param = get("param");
    c.ease = get("ease");
    (void)parse_dbl(get("diff"), &c.diff);
    (void)parse_u64s(get("delay"), &c.delay_per_char);
    (void)parse_u64s(get("time"), &c.time_per_char);
    c.random_delay = get("randomdelay") == "1";
    MessageLayer& l = active_mut();
    if (!c.set_add) {
        const ScetweenMode m = c.mode;
        l.scetween.erase(std::remove_if(l.scetween.begin(), l.scetween.end(),
                                        [m](const ScetweenConfig& x) { return x.mode == m; }),
                         l.scetween.end());
    }
    l.scetween.push_back(std::move(c));
    bump_revision();
}

void TextEngine::set_glyph_config(const std::map<std::string, std::string>& raw) {
    glyph_icon_ = glyph_icon_from_raw(raw);
    bump_revision();
}

void TextEngine::apply_backlog_config(const std::map<std::string, std::string>& p, bool clear) {
    auto get = [&p](const char* k) -> const std::string* {
        const auto it = p.find(k);
        return it == p.end() ? nullptr : &it->second;
    };
    if (const std::string* v = get("allow")) backlog_allow_ = *v != "0";
    if (const std::string* v = get("includefont")) backlog_include_font_ = *v != "0";
    if (clear) backlog_pages_.clear();
    bump_revision();
}

void TextEngine::set_backlog_write_mode(bool mode) {
    backlog_write_mode_ = mode;
    bump_revision();
}

void TextEngine::reveal_next(uint64_t delta_ms) {
    bool any_pending = false;
    for (const auto& [id, l] : layers_) {
        (void)id;
        if (l.reveal_pending) {
            any_pending = true;
            break;
        }
    }
    for (auto& [id, l] : layers_) {
        (void)id;
        if (!l.reveal_pending) continue;
        if (l.char_count == 0) {
            l.reveal_pending = false; // empty push_text: nothing to reveal
            continue;
        }
        const size_t count = l.char_count;
        // 无 scetween → 立即全量
        std::vector<ScetweenConfig> relevant;
        for (const ScetweenConfig& c : l.scetween) {
            if (c.is_entrance() != l.text_hidden) relevant.push_back(c);
        }
        if (relevant.empty()) {
            l.reveal_index = count;
            l.reveal_pending = false;
            continue;
        }
        uint64_t max_delay = 0;
        uint64_t max_total = 0;
        for (const ScetweenConfig& c : relevant) {
            max_delay = std::max(max_delay, c.delay_per_char);
            max_total = std::max(max_total,
                                 (count > 0 ? (uint64_t)(count - 1) : 0) *
                                         c.delay_per_char +
                                     c.time_per_char);
        }
        if (max_delay == 0 && max_total == 0) {
            l.reveal_clock_ms += delta_ms;
            l.reveal_index = count;
            l.reveal_pending = false;
            continue;
        }
        // 首帧不推进；之后单帧最多 kRevealMaxStepMs。
        uint64_t step = delta_ms;
        if (l.reveal_fresh) {
            l.reveal_fresh = false;
            step = 0;
        }
        step = std::min<uint64_t>(step, kRevealMaxStepMs);
        l.reveal_clock_ms += step;
        if (max_delay == 0) {
            l.reveal_index = count;
            if (l.reveal_clock_ms >= max_total) l.reveal_pending = false;
            continue;
        }
        const uint64_t chars = l.reveal_clock_ms / max_delay + 1;
        const size_t ni = std::min((size_t)chars, count);
        if (ni > l.reveal_index) l.reveal_index = ni;
        if (l.reveal_index >= count && l.reveal_clock_ms >= max_total) {
            l.reveal_pending = false;
        }
    }
    // 只有存在待揭示层时才视为画面变化（静态帧跳过不被逐字常驻破坏）
    if (any_pending) bump_revision();
}

bool TextEngine::is_reveal_paced() const {
    for (const auto& [id, l] : layers_) {
        (void)id;
        if (!l.reveal_pending || l.char_count == 0) continue;
        for (const ScetweenConfig& c : l.scetween) {
            if (c.is_entrance() == l.text_hidden) continue;
            if (c.delay_per_char > 0) return true;
        }
    }
    return false;
}

void TextEngine::reveal_all() {
    for (auto& [id, l] : layers_) {
        (void)id;
        // An empty layer can still be flagged pending (an empty push_text on
        // a fresh page armed reveal); nothing to reveal — clear the flag so
        // it can never wedge the click-wait machine (NekoMiko
        // ch1 story clicks stalled on the empty speaker-name layer).
        if (l.char_count == 0) {
            l.reveal_pending = false;
            continue;
        }
        l.reveal_index = l.char_count;
        l.reveal_pending = false;
    }
    bump_revision();
}

bool TextEngine::is_reveal_complete() const {
    for (const auto& [id, l] : layers_) {
        (void)id;
        // zero-char pending layers have nothing to reveal (see reveal_all)
        if (l.reveal_pending && l.char_count > 0) return false;
    }
    return true;
}

std::optional<ClickWaitPlacement> TextEngine::click_wait_placement(bool page_end) const {
    // click_wait_icon_placement：行末/页末图层与偏移；两处都缺省禁用。
    const std::string layer_id = page_end
                                     ? (glyph_icon_.rplayer.empty() ? glyph_icon_.layer
                                                                    : glyph_icon_.rplayer)
                                     : glyph_icon_.layer;
    if (layer_id.empty()) return std::nullopt;
    const double dx = page_end ? glyph_icon_.rpleft : glyph_icon_.left;
    const double dy = page_end ? glyph_icon_.rptop : glyph_icon_.top;
    const std::string lid = active_.value_or(std::string(kDefaultMessageLayer));
    const MessageLayer* l = layer(lid);
    if (!l) return std::nullopt;
    // 本实现不追踪字形末位置（无 glyph 缓存几何），用文本区几何近似：
    // 图标放在该层文本区末尾（left+width, top）。
    ClickWaitPlacement p;
    p.layer_id = layer_id;
    p.left = l->left + (l->width > 0 ? l->width : 0) + dx;
    p.top = l->top + dy;
    return p;
}

std::vector<std::string> TextEngine::visible_content_layers() const {
    std::vector<std::string> out;
    for (const auto& [id, l] : layers_) {
        if (l.text_hidden) continue;
        bool has = false;
        for (const PageUnit& u : l.page) {
            if (u.kind != PageUnit::Kind::Newline && !u.data.empty()) {
                has = true;
                break;
            }
        }
        if (has && l.reveal_index > 0) out.push_back(id);
    }
    return out;
}

bool TextEngine::active_has_content() const {
    const MessageLayer* l = layer(active_layer_id());
    if (!l || l->text_hidden) return false;
    for (const PageUnit& u : l->page) {
        if (u.kind != PageUnit::Kind::Newline && !u.data.empty()) return true;
    }
    return false;
}

static bool is_blank_cp(uint32_t cp) {
    return cp == 0x20 || cp == 0x3000 || cp == 0x9 || cp == 0xA || cp == 0xD;
}

bool TextEngine::page_has_visible_text(std::string_view id) const {
    const MessageLayer* l = layer(id);
    if (!l) return false;
    for (const PageUnit& u : l->page) {
        if (u.kind == PageUnit::Kind::Newline || u.data.empty()) continue;
        size_t i = 0;
        uint32_t cp = 0;
        while (next_codepoint(u.data, &i, &cp)) {
            if (!is_blank_cp(cp)) return true;
        }
    }
    return false;
}

std::optional<std::vector<std::string>> TextEngine::backlog_tags(size_t page,
                                                                 bool allfont) const {
    if (page >= backlog_pages_.size()) return std::nullopt;
    return backlog_pages_[page].reproduction_tags(allfont);
}
std::optional<std::vector<std::string>> TextEngine::message_tags(std::string_view id,
                                                                 bool allfont) const {
    const MessageLayer* l = layer(id);
    if (!l) return std::nullopt;
    std::vector<std::string> out;
    if (allfont && !l->page_font.empty()) {
        out.push_back(BacklogTag{BacklogTag::Kind::Font, "", l->page_font}.get_string());
    }
    for (const BacklogTag& t : l->page_tags) out.push_back(t.get_string());
    return out;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
namespace {
struct InGlyph {
    uint32_t cp = 0;
    double advance = 0;
    double width = 0;
    const FontDesc* font = nullptr;
    bool newline = false;
    bool is_base = false;
    std::string ruby_text;
};
std::vector<uint32_t> cps_of(std::string_view s) {
    std::vector<uint32_t> out;
    size_t i = 0;
    uint32_t cp = 0;
    while (next_codepoint(s, &i, &cp)) out.push_back(cp);
    return out;
}
} // namespace

LaidPage layout_page(const MessageLayer& layer, const MetricsFn& metrics,
                     const LayoutConfig& cfg, void* userdata) {
    LaidPage out;
    std::vector<InGlyph> glyphs;
    std::vector<std::pair<size_t, size_t>> keep_ranges;
    for (const PageUnit& u : layer.page) {
        if (u.kind == PageUnit::Kind::Newline) {
            InGlyph g;
            g.newline = true;
            g.font = &u.font;
            glyphs.push_back(g);
            continue;
        }
        if (u.data.empty()) continue;
        const size_t start = glyphs.size();
        for (const uint32_t cp : cps_of(u.data)) {
            if (cp == '\n') {
                // 防御：若内容内 LF 因任何未来路径绕过 push 层归一
                // （push_text 已把 '\n' 切成 Newline 单元），布局层仍按显式
                // 断行字形处理——强制断行、advance 0、不度量（绘制面已跳过
                // newline 字形；行高/顺序/reveal 语义与 [rt] 单元一致）。
                InGlyph g;
                g.newline = true;
                g.font = &u.font;
                glyphs.push_back(g);
                continue;
            }
            InGlyph g;
            g.cp = cp;
            g.font = &u.font;
            g.is_base = u.kind == PageUnit::Kind::RubyBase;
            g.ruby_text = u.ruby_annotation;
            glyphs.push_back(g);
        }
        if (u.kind == PageUnit::Kind::RubyBase) {
            keep_ranges.push_back({start, glyphs.size()});
        }
    }
    if (glyphs.empty()) return out;

    // 逐字形步进/宽度来自宿主度量；正文行高按 text_line_metrics 公式
    // ：body_height = 层当前字体 size（ab_glyph
    // PxScale::from(sz) 下 sf.height ≡ sz），而非字形 typographic 高度或
    // 1.4×size 系数——否则 spacetop/spacemiddle/spacebottom 完全失效且
    // 行距/字形观感整体偏大（旧实现缺陷）。
    for (InGlyph& g : glyphs) {
        if (g.newline) continue;
        CharMetrics m;
        if (metrics) {
            m = metrics(userdata, *g.font, g.cp);
        } else {
            // 无宿主度量回调（不应发生）：按字号方块近似，仅保布局不塌缩。
            m.advance = g.font->size();
            m.width = g.font->size();
        }
        // 步进不含 kerning 参数:该键仅存/回显,glyph 排版从不读取
        // (layout_glyphs 只累加字形自带 advance_x)。
        g.advance = m.advance > 0.0 ? m.advance : 0.0;
        // 宽度=墨迹框（含 0 墨迹字形如空格 → 不参与换行判定；同 width=px_bounds）。
        g.width = m.width;
    }
    const double body_height = layer.font.size();
    const double spacetop = layer.font.spacetop();
    const double ruby_height = std::max(0.0, layer.font.rubysize());
    const double spacemiddle = layer.font.spacemiddle();
    const double spacebottom = layer.font.spacebottom();
    out.line_height = std::max(1.0, spacetop + ruby_height + spacemiddle + body_height + spacebottom);

    const bool unbounded = layer.width <= 0.0;
    const double line_width = layer.width > 0.0 ? layer.width : 1.0e18;
    const std::vector<uint32_t> pair_cps = cps_of(cfg.indent_pair);
    auto indent_close_of = [&pair_cps](uint32_t c) -> std::optional<uint32_t> {
        for (size_t k = 0; k + 1 < pair_cps.size(); k += 2) {
            if (pair_cps[k] == c) return pair_cps[k + 1];
        }
        return std::nullopt;
    };

    out.glyphs.reserve(glyphs.size());
    double indent_x = 0;
    std::vector<std::pair<uint32_t, double>> indent_stack;
    size_t line_start = 0;
    uint32_t line = 0;
    size_t order = 0;
    while (line_start < glyphs.size()) {
        double cx = indent_x;
        size_t break_at = glyphs.size();
        bool forced = false;
        size_t i = line_start;
        while (i < glyphs.size()) {
            const InGlyph& g = glyphs[i];
            if (g.newline) {
                break_at = i + 1;
                forced = true;
                break;
            }
            if (!unbounded && cx + g.width > line_width && i > line_start) {
                if (contains_cp(cfg.prohibit_head, g.cp)) {
                    cx += g.advance;
                    ++i;
                    continue;
                }
                size_t b = i;
                for (const auto& [rs, re] : keep_ranges) {
                    if (rs < b && b < re && rs > line_start) {
                        b = rs;
                        break;
                    }
                }
                if (b == i && contains_cp(cfg.wordparts, g.cp)) {
                    size_t j = i;
                    while (j > line_start && contains_cp(cfg.wordparts, glyphs[j - 1].cp))
                        --j;
                    if (j > line_start) b = j;
                }
                while (b > line_start + 1 && contains_cp(cfg.prohibit_foot, glyphs[b - 1].cp))
                    --b;
                break_at = b;
                break;
            }
            cx += g.advance;
            ++i;
        }
        cx = indent_x;
        size_t chars_in_line = 0;
        for (size_t k = line_start; k < break_at; ++k) {
            const InGlyph& g = glyphs[k];
            LaidGlyph lg;
            lg.cp = g.cp;
            lg.advance = g.advance;
            lg.width = g.width;
            lg.x = cx;
            lg.line = line;
            lg.font = g.font;
            lg.newline = g.newline;
            lg.has_ruby = g.is_base;
            lg.order = order++;
            out.glyphs.push_back(lg);
            if (g.newline) continue;
            if (const auto close = indent_close_of(g.cp)) {
                const bool within =
                    cfg.indent_range <= 0 || chars_in_line < (size_t)cfg.indent_range;
                if (within && (indent_stack.empty() || cfg.indent_nest)) {
                    indent_stack.push_back({*close, indent_x});
                    indent_x = cx + g.advance;
                }
            } else if (!indent_stack.empty() && indent_stack.back().first == g.cp) {
                indent_x = indent_stack.back().second;
                indent_stack.pop_back();
            }
            cx += g.advance;
            ++chars_in_line;
        }
        ++line;
        line_start = break_at;
        if (!forced && !unbounded && break_at >= glyphs.size()) break;
    }

    // 行对齐（align_layout）：align 键 center/right/
    // equalize 时按行内容宽（首字形 x .. 末字形 x+advance）对行宽居中/右/均排。
    // 仅当层宽 >0 且当前层字体声明了非左对齐时生效（TextAlignment::from）。
    if (!out.glyphs.empty() && layer.width > 0.0) {
        const std::string align = layer.font.get_or("align", "left");
        if (align != "left" && !align.empty()) {
            const double lw = layer.width;
            // 收集每行非换行字形的首/末下标
            std::map<uint32_t, std::pair<size_t, size_t>> line_bounds; // first,last
            for (size_t i = 0; i < out.glyphs.size(); ++i) {
                if (out.glyphs[i].newline) continue;
                const uint32_t ln = out.glyphs[i].line;
                auto it = line_bounds.find(ln);
                if (it == line_bounds.end()) line_bounds[ln] = {i, i};
                else it->second.second = i;
            }
            auto apply_off = [&](const std::pair<size_t, size_t>& b, double off) {
                for (size_t i = b.first; i <= b.second; ++i) out.glyphs[i].x += off;
            };
            for (const auto& [ln, b] : line_bounds) {
                const double left = out.glyphs[b.first].x;
                const double right = out.glyphs[b.second].x + out.glyphs[b.second].advance;
                const double content = (right - left) > 0.0 ? (right - left) : 0.0;
                const double spare = (lw - content) > 0.0 ? (lw - content) : 0.0;
                const size_t n = b.second - b.first + 1;
                if (align == "center") {
                    apply_off(b, spare * 0.5 - left);
                } else if (align == "right") {
                    apply_off(b, spare - left);
                } else if (align == "equalize" && n > 1) {
                    const double step = spare / double(n - 1);
                    for (size_t i = b.first; i <= b.second; ++i)
                        out.glyphs[i].x += step * double(i - b.first) - left;
                } else if (align == "equalize") {
                    apply_off(b, -left);
                }
            }
        }
    }

    for (const auto& [rs, re] : keep_ranges) {
        if (rs >= re || rs >= out.glyphs.size()) continue;
        const LaidGlyph& first = out.glyphs[rs];
        const LaidGlyph* last = &first;
        for (size_t idx = rs + 1; idx < re && idx < out.glyphs.size(); ++idx) {
            if (out.glyphs[idx].line == first.line) last = &out.glyphs[idx];
        }
        LaidRuby r;
        r.line = first.line;
        r.x = first.x;
        // 正文区间右端 = 末字形笔端（x+advance；同 base_x1 =
        // laid[last].x + text_buffer[last].advance_x）。
        r.width = (last->x + last->advance) - first.x;
        r.text = glyphs[rs].ruby_text;
        const FontDesc* bf = glyphs[rs].font;
        r.size = 0;
        if (bf) {
            const std::string* rs2 = bf->get("rubysize");
            if (rs2) (void)parse_dbl(*rs2, &r.size);
            if (r.size <= 0) r.size = bf->size() * 0.5;
        }
        out.rubies.push_back(std::move(r));
    }
    return out;
}

} // namespace oa::render
