#pragma once
// oa::render — Artemis message-layer text engine: message-layer
// stack/switch/pop, (scetween), backlog reproduction tags, glyph click-wait
// icon config, and the pure glyph layout stage (wrap/prohibit/wordparts/ruby
// keep-ranges).
//
// Semantic summary:
//   - DEFAULT_MESSAGE_LAYER "adv01", default font size 40
//   - chgmsg id 缺省 → 匿名 id chgmsg_<nanos:x>_<serial> (switch Chgmsg)
//   - switch_message_layer / apply_font_settings / font_init/close/default
//     (layer stack); geometry keys left/top/width/height
//   - push_text / push_line_break(omit) / push_page_break
//   - reveal：MessageLayer reveal_index/pending/clock + reveal_next
//     + reveal_all；新内容（空缓冲后首推）
//     置 pending；scetween 空 → 立即全量
//   - scetween 配置（ScetweenConfig；type/mode/param/ease/diff/
//     delay/time/randomdelay）
//   - backlog：页内 page_tags + BacklogTag（text/backlog）、rp 时按
//     should_store(backlog 参数或 write_mode) 入 Backlog.push_page
//   - glyph 图标配置（GlyphIconConfig）
//   - 排版算法 layout_glyphs
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::render {

inline constexpr std::string_view kDefaultMessageLayer = "adv01";
inline constexpr double kDefaultFontSize = 40.0;
/// 缺省字体文件候选（runtime load_default_font）。
inline constexpr std::string_view kDefaultFontFile = "font/sourcehansans-medium.otf";
inline constexpr size_t kBacklogMaxPages = 100;

// ---------------------------------------------------------------------------
// Font settings: 保形参数表 + 少量 typed 取用（FontDesc）
// ---------------------------------------------------------------------------
struct FontDesc {
    std::map<std::string, std::string> raw;
    void merge_raw(const std::map<std::string, std::string>& p);
    const std::string* get(std::string_view key) const;
    std::string get_or(std::string_view key, const std::string& fallback) const;
    double size() const;
    std::string face() const;
    std::string color() const;
    double spacetop() const;
    double spacemiddle() const;
    double spacebottom() const;
    double rubysize() const;
};

// ---------------------------------------------------------------------------
// Page units：一页文本的逻辑单元流（排版输入）
// ---------------------------------------------------------------------------
struct PageUnit {
    enum class Kind { Text, Newline, RubyBase };
    Kind kind = Kind::Text;
    std::string data;
    std::string ruby_annotation;
    FontDesc font;
};

// ---------------------------------------------------------------------------
// scetween / backlog 再现 / glyph 图标
// ---------------------------------------------------------------------------
enum class ScetweenMode {
    In, Out, Show, Hide, BacklogDownIn, BacklogDownOut, BacklogUpIn, BacklogUpOut,
};

struct ScetweenConfig {
    ScetweenMode mode = ScetweenMode::In;
    bool set_add = false; // [scetween mode=add] vs init
    std::string param;    // "alpha"/"left"/...
    std::string ease;
    double diff = 0;
    uint64_t delay_per_char = 0; // [scetween delay=]
    uint64_t time_per_char = 0;  // [scetween time=]
    bool random_delay = false;
    bool is_entrance() const {
        return mode == ScetweenMode::In || mode == ScetweenMode::Show ||
               mode == ScetweenMode::BacklogDownIn || mode == ScetweenMode::BacklogUpIn;
    }
};

/// backlog 页内再现标签（BacklogTag）。
struct BacklogTag {
    enum class Kind { Text, LineBreak, Font, RubyStart, RubyEnd };
    Kind kind = Kind::Text;
    std::string text;
    std::map<std::string, std::string> font;
    std::string get_string() const;
};

struct BacklogPage {
    std::map<std::string, std::string> page_font;
    std::vector<BacklogTag> tags;
    bool has_text() const;
    std::vector<std::string> reproduction_tags(bool allfont) const;
};

struct GlyphIconConfig {
    std::string layer;    // 行末图标层（缺省禁用）
    std::string rplayer;  // 页末图标层（缺省=layer）
    double left = 0, top = 0, rpleft = 0, rptop = 0;
    bool homing = false;
    static GlyphIconConfig from_raw(const std::map<std::string, std::string>& raw);
};

struct ClickWaitPlacement {
    std::string layer_id;
    double left = 0;
    double top = 0;
};

// ---------------------------------------------------------------------------
// Layout（纯排版）
// ---------------------------------------------------------------------------
struct CharMetrics {
    double advance = 0;
    double width = 0;
    double height = 0;
};
using MetricsFn = std::function<CharMetrics(void* userdata, const FontDesc&, uint32_t cp)>;

struct LayoutConfig {
    std::string prohibit_head;
    std::string prohibit_foot;
    std::string wordparts;
    std::string indent_pair;
    int indent_range = 0;
    bool indent_nest = false;
};

struct LaidGlyph {
    uint32_t cp = 0;
    double advance = 0;
    double width = 0;
    double x = 0;
    uint32_t line = 0;
    const FontDesc* font = nullptr;
    bool newline = false;
    bool has_ruby = false;
    size_t order = 0; // 页内字形序号（reveal_index 过滤用）
};
struct LaidRuby {
    uint32_t line = 0;
    double x = 0;
    double width = 0;
    std::string text;
    double size = 0;
};
struct LaidPage {
    std::vector<LaidGlyph> glyphs;
    std::vector<LaidRuby> rubies;
    double line_height = 0;
};

struct MessageLayer {
    std::string id;
    bool layered = false;
    /// 该层是否被 [font] 排版键（left/top/width/height 任一）显式配置过绘制
    /// 盒。字体几何是 Artemis 文本的唯一落点来源（每张字表自带 rect）;
    /// 从未排版过的层 = 无绘制盒,只能画在脚本显式创建的节点上
    /// （is_message_layer_drawable 门槛,孤儿打印不绘制）。
    bool positioned = false;
    double left = 0, top = 0, width = 0, height = 0;
    FontDesc font;
    std::vector<FontDesc> font_stack;
    bool text_hidden = false;
    std::vector<PageUnit> page;
    uint64_t generation = 0;
    // reveal/scetween/backlog
    uint64_t reveal_index = 0;
    bool reveal_pending = false;
    uint64_t reveal_clock_ms = 0;
    std::vector<ScetweenConfig> scetween;
    std::vector<BacklogTag> page_tags;
    std::map<std::string, std::string> page_font;
    size_t char_count = 0; // 页单元展平字形数（reveal 上限）
};

// ---------------------------------------------------------------------------
// TextEngine：消息层栈 + 各层字体/页面状态 + 事件归约
// ---------------------------------------------------------------------------
class TextEngine {
public:
    TextEngine();

    void clear_scene();
    /// 把活动层锚回"当前持有可见页内容的层"：页面点击等待释放时调用
    /// （advance_wait）。读档/UI 页会话残链（存档/读档页
    /// chgmsg 平衡对以 load 清空的栈为底弹栈）会把活动层留在 UI 层
    /// （如 500.pageno），而剧本翻页分派（script.asb 的 chgmsg/pop/rp 清页
    /// 模式）假定 '@' 停驻释放时活动层 == 剧情文本层——错层导致清页清错
    /// 层、新文本叠在旧页后且 reveal 冻结（NekoMiko 读档后"剧情推进但文字
    /// 不动"）。锚定同时清空消息层栈，使后续 chgmsg push/pop 平衡对以剧情
    /// 层为底。无可见文本内容层时不动。
    void anchor_active_to_content();
    const FontDesc& default_font() const { return default_font_; }

    // [chgmsg]
    void switch_layer(std::optional<std::string> id, bool stack,
                      std::optional<int> layered);
    void pop_layer();
    // font 族
    void apply_font(const std::map<std::string, std::string>& p);
    void font_init();
    void font_close();
    void font_default(const std::map<std::string, std::string>& p);
    // 文本：push_text 把内容按 '\n' 归一为 (Text 段 | Newline 单元) 序列
    // （内容内 LF == [rt] 断行语义；CRLF 的 '\r' 丢弃）。
    void push_text(const std::string& content);
    void line_break(bool omit_blank_line);
    void page_break(std::optional<int> backlog_param);
    // ruby
    void ruby_start(const std::string& annotation);
    void ruby_end();
    // scein/sceout
    void set_text_hidden(bool hidden);
    // 排版配置
    void set_prohibit(const std::string& head, const std::string& foot);
    void set_wordparts(const std::string& parts);
    void set_indent(const std::string& pair);
    // [scetween]
    void apply_scetween(const std::map<std::string, std::string>& p);
    // [glyph]
    void set_glyph_config(const std::map<std::string, std::string>& raw);
    // [backlog]/[writebacklog]
    void apply_backlog_config(const std::map<std::string, std::string>& p, bool clear);
    void set_backlog_write_mode(bool mode);
    // reveal 驱动
    void reveal_next(uint64_t delta_ms);
    void reveal_all();
    bool is_reveal_complete() const;
    // click_wait 图标位置（行末/页末）
    std::optional<ClickWaitPlacement> click_wait_placement(bool page_end) const;

    const LayoutConfig& layout_config() const { return layout_cfg_; }

    // --- 查询 ---
    std::string active_layer_id() const;
    const MessageLayer* layer(std::string_view id) const;
    uint64_t revision() const { return revision_; }
    std::vector<std::string> visible_content_layers() const;
    bool active_has_content() const;
    const GlyphIconConfig& glyph_icon() const { return glyph_icon_; }
    // 页面是否含非空白字符（诊断/冒烟：basefont 空白测量层排除）
    bool page_has_visible_text(std::string_view id) const;
    // backlog 查询
    size_t backlog_size() const { return backlog_pages_.size(); }
    std::optional<std::vector<std::string>> backlog_tags(size_t page, bool allfont) const;
    std::optional<std::vector<std::string>> message_tags(std::string_view id,
                                                         bool allfont) const;

    // --- 内部/测试观察 ---
    size_t text_layers() const { return layers_.size(); }
    const std::map<std::string, MessageLayer>& layers() const { return layers_; }
    const std::vector<std::string>& layer_stack() const { return layer_stack_; }

private:
    MessageLayer& active_mut();
    void bump_revision();
    std::string anon_id();
    size_t recompute_char_count(MessageLayer& l) const;
    std::vector<BacklogTag> page_tags_of(const MessageLayer& l) const;
    std::optional<std::string> active_;
    std::vector<std::string> layer_stack_;
    std::map<std::string, MessageLayer> layers_;
    FontDesc default_font_;
    uint64_t revision_ = 0;
    bool ruby_open_ = false;
    std::string ruby_annotation_;
    uint64_t anonymous_serial_ = 0;
    LayoutConfig layout_cfg_;
    // session state
    std::vector<BacklogPage> backlog_pages_;
    bool backlog_allow_ = true;
    bool backlog_write_mode_ = false;
    bool backlog_include_font_ = true;
    GlyphIconConfig glyph_icon_;
};

bool next_codepoint(std::string_view s, size_t* i, uint32_t* cp);
LaidPage layout_page(const MessageLayer& layer, const MetricsFn& metrics,
                     const LayoutConfig& cfg, void* userdata);

} // namespace oa::render
