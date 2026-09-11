// research/113 save-path hardening tests: the "save compat" regression net.
//
// 用户裁定（2026-09，存档定义阶段）：格式未冻结 ⇒ **跨版本兼容不做**（旧档前缀
// 语义、"读旧档不得黑半屏"之类的跨版本期望都不是验收项；唯一保留的是 decode 的
// 版本门 + 未来跨版本分派点，见 runtime_internal.h 的版本门契约）。本文件的语料
// 因此以**同版本内的边界/畸形输入**为主：
//   S1 缺父 id 的层 + 不可解析资源名（父路径物化 / file 逐字保留，恢复期不解析资源）
//   S2 裁切组（intermediate_render=2 + clip）带非零 left/top/zoom：读档后裁切窗按
//      **当前几何语义**（= renderer.cpp world_aabb_local 的同一公式，本文件逐字
//      复刻：clip 局部矩形经层自身世界变换映射）计算，且不残留读档前同 id 层的
//      旧几何（"上半屏"式裁切的回归网）
//   S3 空 scene 快照 / 仅 local 变量的档：行为有定义（清场 + 只恢复脚本/Lua 态）
//   S4 畸形/重复层 id：显式跳过 + 报告，不物化空名节点、不崩溃
//   S5 旧保留命名空间前缀（pre-108 形态）：**记录性**用例（不断言兼容行为，只钉
//      当前行为 = 逐字保留，为未来跨版本预留）
//   S6 [C] 桶读档清场：在飞 [alldelete] 不得抹掉刚恢复的场景（缺-6）；click-wait
//      图标不得物化幽灵层（缺-7）；skip 不得跨读档存活（缺-8）；script_status==4
//      不得让读档后剧本停摆（缺-9）
//   S7 ② 桶特判保留：interpreter tag 队列跨读档存活（缺-5，FPM load_next 依赖）
//   S8 失败原子性：文件缺失 / 损坏 / 位置预检失败 ⇒ 运行时状态一个字节不动
//      （不存在"场景已清、快照已灌、位置恢复失败"的半恢复）
//   S9 事件泵确定性：读档派发期间由恢复链新产生的事件不丢（迭代器安全化）
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/fs/fs.h"
#include "core/runtime/runtime_save.h"
#include "core/render/layer.h"
#include "core/runtime/runtime.h"
#include "core/runtime/runtime_save.h"

namespace {

int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// ---------------------------------------------------------------------------
// Harness (MemFs + MemSaveStore; mirrors save_domain_test / p1c2_control_test)
// ---------------------------------------------------------------------------
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

class MemSaveStore final : public oa::runtime::SaveStore {
public:
    std::map<std::string, std::vector<uint8_t>> files;
    bool write(const std::string& rel, const std::vector<uint8_t>& d) override {
        files[rel] = d;
        return true;
    }
    std::optional<std::vector<uint8_t>> read(const std::string& rel) const override {
        const auto it = files.find(rel);
        if (it == files.end()) return std::nullopt;
        return it->second;
    }
    bool remove(const std::string& rel) override {
        const auto it = files.find(rel);
        if (it == files.end()) return false;
        files.erase(it);
        return true;
    }
    bool exists(const std::string& rel) const override { return files.count(rel) > 0; }
};

// 默认剧本（fixture 的位置都落在这里；行号写在注释里并被 current_line 引用）：
//   0 [lyc id="pre" file="pre.png"]   <- 读档前的现场标记层（S1/S8 用）
//   1 [print data="page one"]
//   2 [wt0]                           <- 点击停驻（Generic0；[wt] 是 0ms 的 Timed
//                                        等待，会被同一 tick 直接放行，不是点击停驻）
//   3 [print data="page two"]
//   4 [stop]
const char* kStory = R"(*main
[lyc id="pre" file="pre.png"]
[print data="page one"]
[wt0]
[print data="page two"]
[stop]
)";

constexpr size_t kLineClickWait = 2;
constexpr size_t kLineStop = 4;

struct Harness {
    std::shared_ptr<MemFs> fs = std::make_shared<MemFs>();
    std::shared_ptr<MemSaveStore> store = std::make_shared<MemSaveStore>();
    std::unique_ptr<oa::runtime::GameRuntime> rt;

    Harness(std::string boot = "[call file=\"story.iet\" label=\"main\"]\n",
            std::string story = kStory) {
        fs->files["system.ini"] =
            "[WINDOWS]\nWIDTH = 1280\nHEIGHT = 720\nFPS = 60\nCHARSET = UTF-8\n"
            "BOOT = system/first.iet\nSAVEPATH = save\n";
        fs->files["system/first.iet"] = "*main\n" + std::move(boot);
        fs->files["story.iet"] = std::move(story);
        rt = std::make_unique<oa::runtime::GameRuntime>(fs);
        rt->set_save_store(store);
        rt->open_project("windows");
        rt->boot_project();
    }
    void tick(int mx = -5, int my = -5, bool click = false) {
        oa::runtime::FrameInput in;
        in.mouse_x = mx;
        in.mouse_y = my;
        in.left_down = click;
        in.left_click_edge = click;
        rt->tick(16, in);
    }
    void tick_ms(uint64_t ms) { rt->tick(ms, oa::runtime::FrameInput{}); }
    void warm(int n = 3) {
        for (int i = 0; i < n; ++i) tick();
    }
    void run_lua(const char* code) {
        rt->interpreter().lua_bridge().run_code(code, "save_compat_test");
    }
    const oa::render::Layer* layer(const std::string& id) const { return rt->scene().find(id); }
    bool has_layer(const std::string& id) const { return layer(id) != nullptr; }
    size_t count_layer(const std::string& id) const {
        size_t n = 0;
        for (const oa::render::Layer* l : rt->scene().draw_order())
            if (l->id == id) ++n;
        return n;
    }
    std::string wait_str() const {
        const oa::runtime::WaitReason* w = rt->current_wait();
        if (!w) return "none";
        switch (w->kind) {
            case oa::runtime::WaitReason::Kind::Generic: return "generic";
            case oa::runtime::WaitReason::Kind::Generic0: return "wt0";
            case oa::runtime::WaitReason::Kind::Timed: return "timed";
            case oa::runtime::WaitReason::Kind::Stop: return "stop";
            case oa::runtime::WaitReason::Kind::Se: return "se";
            case oa::runtime::WaitReason::Kind::VideoLayer: return "video";
            case oa::runtime::WaitReason::Kind::ScenarioTween: return "scenario";
            case oa::runtime::WaitReason::Kind::KeyWait: return "key";
        }
        return "?";
    }
    std::pair<std::string, size_t> pos() const {
        const std::string* s = rt->interpreter().current_script();
        return {s ? *s : std::string(), rt->interpreter().current_line()};
    }
    void install(const char* name, const oa::runtime::SaveData& d) {
        const std::string doc = d.encode();
        store->files["save/" + std::string(name)] =
            std::vector<uint8_t>(doc.begin(), doc.end());
    }
};

// ---------------------------------------------------------------------------
// Fixture construction
// ---------------------------------------------------------------------------
std::map<std::string, std::string> props(
    const std::initializer_list<std::pair<const char*, const char*>>& kv) {
    std::map<std::string, std::string> m;
    for (const auto& [k, v] : kv) m[k] = v;
    return m;
}

oa::runtime::LayerSnap lay(const std::string& id, std::map<std::string, std::string> p) {
    oa::runtime::LayerSnap s;
    s.id = id;
    s.props = std::move(p);
    return s;
}

/// 一份"同版本内合法"的场景档；`line` 选停驻行。
oa::runtime::SaveData scene_fixture(std::vector<oa::runtime::LayerSnap> layers, size_t line,
                                 std::map<std::string, std::string> root = {}) {
    oa::runtime::SaveData d;
    d.current_script = "story.iet";
    d.current_line = line;
    d.has_scene = true;
    d.root_props = std::move(root);
    d.layers = std::move(layers);
    d.local_variables["fx"] = oa::runtime::Value::make_string("loaded");
    return d;
}

/// renderer.cpp world_aabb_local 的逐字复刻（组的裁切窗 = clip 局部矩形经**层
/// 自身**世界变换映射出的世界矩形；角点 floor/ceil 取整）。断言用它 = "引擎当前
/// 几何语义"的可执行定义（该函数是 RenderEngine 私有的，测试侧按公式复刻）。
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};
std::optional<Rect> crop_window(const oa::render::Compositor& sc,
                               const oa::render::Layer& l, double x, double y, double w,
                               double h) {
    oa::render::Affine2 t;
    if (!sc.world_transform(l.id, &t)) return std::nullopt;
    const double cx[4] = {x, x + w, x, x + w};
    const double cy[4] = {y, y, y + h, y + h};
    double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300;
    for (int i = 0; i < 4; ++i) {
        double px = 0, py = 0;
        t.transform_point(cx[i], cy[i], &px, &py);
        minx = std::min(minx, px);
        maxx = std::max(maxx, px);
        miny = std::min(miny, py);
        maxy = std::max(maxy, py);
    }
    const double x0 = std::floor(minx), y0 = std::floor(miny);
    const double x1 = std::ceil(maxx), y1 = std::ceil(maxy);
    if (x1 <= x0 || y1 <= y0) return std::nullopt;
    return Rect{int(x0), int(y0), int(x1 - x0), int(y1 - y0)};
}

std::optional<Rect> layer_clip_window(const oa::render::Compositor& sc,
                                     const oa::render::Layer& l) {
    const auto it = l.props.find("clip");
    if (it == l.props.end()) return std::nullopt;
    double c[4] = {0, 0, 0, 0};
    if (std::sscanf(it->second.c_str(), "%lf,%lf,%lf,%lf", &c[0], &c[1], &c[2], &c[3]) != 4)
        return std::nullopt;
    return crop_window(sc, l, c[0], c[1], c[2], c[3]);
}

bool rect_is(const std::optional<Rect>& r, int x, int y, int w, int h) {
    return r && r->x == x && r->y == y && r->w == w && r->h == h;
}

std::string prop_of(const oa::render::Layer* l, const char* k) {
    if (!l) return "<no layer>";
    const auto it = l->props.find(k);
    return it == l->props.end() ? std::string("<absent>") : it->second;
}

// ---------------------------------------------------------------------------
// S1 — 缺父 id 的层 + 不可解析资源名（同版本边界）
// ---------------------------------------------------------------------------
void test_missing_parent_and_unresolvable_file() {
    Harness h;
    h.warm();
    check(h.has_layer("pre"), "S1 setup: pre-load marker layer exists");

    auto d = scene_fixture({lay("bg", props({{"file", "bg.png"}, {"left", "10"}, {"top", "20"}})),
                            lay("500.x", props({{"file", "no_such_asset.png"},
                                                {"left", "1"},
                                                {"top", "2"}}))},
                           kLineStop);
    h.install("s1.dat", d);
    check(h.rt->load_game("s1.dat", 0) == oa::runtime::LoadResult::Ok, "S1 load returns Ok");

    h.tick(); // 恢复位置 = [stop]
    check(h.has_layer("bg") && h.has_layer("500.x"),
          "S1 both snapshot layers exist after the load");
    // 缺父 id：父路径由 Compositor::create 的前缀物化建立（既有显式路径语义），
    // 父节点是空容器（不带任何虚构 props）。
    const oa::render::Layer* parent = h.layer("500");
    check(parent != nullptr, "S1 missing parent id '500' materialized by the path rule");
    check(parent && parent->props.empty(), "S1 materialized parent carries no invented props");
    // 不可解析资源名：恢复期逐字保留（资源解析是绘制期的事，失败要容忍，不在这里
    // 绑定/改名/报错）。
    const oa::render::Layer* child = h.layer("500.x");
    check(child && prop_of(child, "file") == "no_such_asset.png",
          "S1 unresolvable file prop kept verbatim (no resolution at restore time)");
    const auto fx = h.rt->interpreter().variables().get("fx");
    check(fx && fx->kind == oa::runtime::ValueKind::String && fx->str_val == "loaded",
          "S1 local variables restored");
    check(h.pos().second == kLineStop, "S1 position restored to the fixture line");
    check(!h.has_layer("pre"), "S1 pre-load marker layer replaced by the snapshot");
}

// ---------------------------------------------------------------------------
// S2 — 裁切组非零几何：裁窗 = 当前几何语义（"上半屏"式残留的回归网）
// ---------------------------------------------------------------------------
void test_crop_group_geometry_semantics() {
    // 读档前：同 id 层带**相反**的几何（"500.crop" top=720 zoom=200 → 世界矩形
    // {0,720,2560,1440}）。若读档残留旧几何/旧裁窗，下面的断言会红。
    const char* story = R"(*main
[lyc id="500" file=""]
[lyprop id="500" intermediate_render="2" clip="0,0,1280,720" left="0" top="0"]
[lyc id="500.crop" file=""]
[lyprop id="500.crop" intermediate_render="2" clip="0,0,1280,720" left="0" top="720" zoom="200"]
[lyc id="pre" file="pre.png"]
[stop]
)";
    Harness h("[call file=\"story.iet\" label=\"main\"]\n", story);
    h.warm();
    const oa::render::Layer* pre_crop = h.layer("500.crop");
    check(pre_crop != nullptr, "S2 setup: pre-load '500.crop' exists");
    check(rect_is(pre_crop ? layer_clip_window(h.rt->scene(), *pre_crop) : std::nullopt, 0,
                  720, 2560, 1440),
          "S2 setup: pre-load crop window follows the pre-load geometry");

    auto d = scene_fixture({lay("500", props({{"intermediate_render", "2"},
                                             {"clip", "0,0,1280,720"},
                                             {"left", "0"},
                                             {"top", "0"}})),
                            lay("500.crop", props({{"intermediate_render", "2"},
                                                   {"clip", "0,0,1280,720"},
                                                   {"left", "0"},
                                                   {"top", "360"},
                                                   {"zoom", "50"}}))},
                           kLineStop);
    h.install("s2.dat", d);
    check(h.rt->load_game("s2.dat", 0) == oa::runtime::LoadResult::Ok, "S2 load Ok");
    h.tick();

    const oa::render::Layer* clean = h.layer("500");
    const oa::render::Layer* moved = h.layer("500.crop");
    check(clean && moved, "S2 both crop groups restored");
    if (!clean || !moved) return;
    // props 逐字 = 存档值（引擎不注入派生键、不带上一个场景的值）。
    check(prop_of(clean, "left") == "0" && prop_of(clean, "top") == "0",
          "S2 group '500' keeps the saved offset");
    check(prop_of(moved, "top") == "360" && prop_of(moved, "zoom") == "50",
          "S2 group '500.crop' keeps the saved geometry (no stale pre-load values)");
    // 裁窗 = 当前语义：clip 局部矩形 × 层自身世界变换。
    check(rect_is(layer_clip_window(h.rt->scene(), *clean), 0, 0, 1280, 720),
          "S2 full-stage clip on a clean group yields the full stage window");
    check(rect_is(layer_clip_window(h.rt->scene(), *moved), 0, 360, 640, 360),
          "S2 non-zero geometry maps deterministically (top=360 zoom=50 -> the saved "
          "lower-half window; no 'upper half' residue)");
    // 第二次读档换一份几何：窗口随之改变，且上一份的层彻底消失 ⇒ 不存在跨读档
    // 残留的裁切状态。
    auto d2 = scene_fixture({lay("500.crop", props({{"intermediate_render", "2"},
                                                   {"clip", "0,0,1280,720"},
                                                   {"left", "0"},
                                                   {"top", "0"},
                                                   {"zoom", "100"}}))},
                            kLineStop);
    h.install("s2b.dat", d2);
    check(h.rt->load_game("s2b.dat", 0) == oa::runtime::LoadResult::Ok, "S2b load Ok");
    h.tick();
    const oa::render::Layer* back = h.layer("500.crop");
    check(back && rect_is(layer_clip_window(h.rt->scene(), *back), 0, 0, 1280, 720),
          "S2b a second load resets the crop window to the new fixture geometry");
    // 新快照没有 "500" 这个节点，但 "500.crop" 的父路径会被前缀物化重建（空容器）
    // —— 旧场景里 "500" 的**属性**没有回来（这正是要断言的"不残留"）。
    const oa::render::Layer* parent = h.layer("500");
    check(parent && parent->props.empty(),
          "S2b the old '500' subtree is gone (parent only re-materialized empty by the "
          "path rule)");
}

// ---------------------------------------------------------------------------
// S3 — 空 scene / 仅 local 变量（行为有定义）
// ---------------------------------------------------------------------------
void test_empty_scene_and_local_only() {
    {
        Harness h;
        h.warm();
        auto d = scene_fixture({}, kLineStop);
        h.install("s3a.dat", d);
        check(h.rt->load_game("s3a.dat", 0) == oa::runtime::LoadResult::Ok,
              "S3a empty-scene snapshot loads Ok");
        h.tick();
        check(h.rt->scene().size() == 0, "S3a empty snapshot -> empty scene (defined)");
        check(!h.has_layer("pre"), "S3a pre-load layers are gone");
        check(h.pos().second == kLineStop, "S3a position still restored");
        check(h.wait_str() == "stop", "S3a [stop] re-established from the restored line");
    }
    {
        Harness h;
        h.warm();
        oa::runtime::SaveData d; // 无 scene / 无 audio：只有 local 变量 + 位置
        d.current_script = "story.iet";
        d.current_line = kLineStop;
        d.local_variables["only"] = oa::runtime::Value::make_int(7);
        h.install("s3b.dat", d);
        check(h.rt->load_game("s3b.dat", 0) == oa::runtime::LoadResult::Ok,
              "S3b local-vars-only snapshot loads Ok");
        h.tick();
        // 定义行为：读档的 [C] 清场是无条件的（引擎域复位），但没有场景快照就
        // 不重放任何层 ⇒ 场景为空 + 脚本/变量恢复（契约在 runtime_internal.h）。
        check(h.rt->scene().size() == 0,
              "S3b scene-less snapshot leaves the scene cleared (documented)");
        const auto v = h.rt->interpreter().variables().get("only");
        check(v && v->kind == oa::runtime::ValueKind::Int && v->int_val == 7,
              "S3b local variable restored");
        check(h.pos().second == kLineStop, "S3b position restored");
    }
}

// ---------------------------------------------------------------------------
// S4 — 畸形 / 重复层 id：显式处理（跳过 + 报告），不物化、不崩溃
// ---------------------------------------------------------------------------
void test_malformed_and_duplicate_ids() {
    Harness h;
    h.warm();
    auto d = scene_fixture({lay("", props({{"file", "x.png"}})),
                            lay(".lead", props({{"file", "x.png"}})),
                            lay("a..b", props({{"file", "x.png"}})),
                            lay("dup", props({{"file", "one.png"}})),
                            lay("dup", props({{"file", "two.png"}}))},
                           kLineStop);
    h.install("s4.dat", d);
    check(h.rt->load_game("s4.dat", 0) == oa::runtime::LoadResult::Ok,
          "S4 malformed-id snapshot still loads (tolerant, log-only self-check)");
    h.tick();
    bool empty_named = false;
    for (const oa::render::Layer* l : h.rt->scene().draw_order())
        if (l->id.empty()) empty_named = true;
    check(!empty_named, "S4 no empty-named node materialized");
    check(!h.has_layer(".lead"),
          "S4 leading-dot id skipped (would materialize an empty parent)");
    check(!h.has_layer("a..b"), "S4 empty path segment id skipped");
    check(h.count_layer("dup") == 1, "S4 duplicate id yields exactly one node");
    const oa::render::Layer* dup = h.layer("dup");
    check(dup && prop_of(dup, "file") == "one.png",
          "S4 duplicate id: the first occurrence wins (deterministic)");
    check(h.rt->scene().size() == 1, "S4 only the valid unique layer survives");
}

// ---------------------------------------------------------------------------
// S5 — 旧保留命名空间前缀（pre-108 形态）：记录性 / 未来跨版本预留
// ---------------------------------------------------------------------------
void test_legacy_reserved_prefix_record_only() {
    // 用户裁定：跨版本兼容现在不做。本用例**不断言兼容行为**，只把当前行为钉死
    // （逐字保留 + 不崩溃 + 自检有报告），作为将来做跨版本时的对照基线。
    Harness h;
    h.warm();
    auto d = scene_fixture(
        {lay("host_frame", props({{"file", "__video_layer__:500.z.mv"}, {"left", "3"}}))},
        kLineStop);
    h.install("s5.dat", d);
    check(h.rt->load_game("s5.dat", 0) == oa::runtime::LoadResult::Ok,
          "S5 (future) legacy-prefix snapshot loads Ok (defined: verbatim)");
    h.tick();
    const oa::render::Layer* l = h.layer("host_frame");
    check(l && prop_of(l, "file") == "__video_layer__:500.z.mv",
          "S5 (future) legacy reserved file prop kept verbatim (no migration)");
    check(h.rt->scene().size() == 1, "S5 (future) no extra/ghost node from the legacy prop");
}

// ---------------------------------------------------------------------------
// S6a — 缺-6：在飞 [alldelete] 不得抹掉刚恢复的场景
// ---------------------------------------------------------------------------
void test_inflight_alldelete_cancelled_by_load() {
    Harness h;
    h.warm();
    h.rt->begin_all_delete(1000); // 在飞淡出（1s 后 finish_all_delete 清场景）
    h.tick_ms(500);               // 走到淡出中途（fade 从 1.0 递减）
    check(h.rt->all_delete_fade() < 1.0, "S6a setup: [alldelete] fade in flight");

    auto d = scene_fixture({lay("after_load", props({{"file", "bg.png"}}))}, kLineStop);
    h.install("s6a.dat", d);
    check(h.rt->load_game("s6a.dat", 0) == oa::runtime::LoadResult::Ok, "S6a load Ok");
    check(h.rt->all_delete_fade() == 1.0,
          "S6a the in-flight [alldelete] fade is cancelled by the load");
    // 越过淡出到期时刻：若计时器跨读档存活，这一 tick 会 finish_all_delete 把刚
    // 恢复的场景整片清掉。
    h.tick_ms(1500);
    check(h.has_layer("after_load"),
          "S6a the restored scene survives past the old fade deadline (缺-6)");
    check(h.rt->scene().size() == 1, "S6a scene is exactly the restored snapshot");
}

// ---------------------------------------------------------------------------
// S6b — 缺-7：click-wait 图标不得把旧 id 物化成幽灵层
// ---------------------------------------------------------------------------
void test_no_ghost_click_wait_icon() {
    Harness h("[glyph layer=\"waiticon\"]\n[call file=\"story.iet\" label=\"main\"]\n");
    h.warm(4);
    check(h.wait_str() == "wt0", "S6b setup: parked on the click wait ([wt0])");
    check(h.has_layer("waiticon"),
          "S6b setup: click-wait icon layer materialized while waiting");
    const oa::render::Layer* icon = h.layer("waiticon");
    check(icon && icon->visible != 0.0, "S6b setup: the icon is visible");

    // 读档到一个**没有** waiticon 的场景，且停驻在 [stop]（不是 click wait）。
    auto d = scene_fixture({lay("scene_a", props({{"file", "a.png"}}))}, kLineStop);
    h.install("s6b.dat", d);
    check(h.rt->load_game("s6b.dat", 0) == oa::runtime::LoadResult::Ok, "S6b load Ok");
    h.tick();
    check(h.wait_str() == "stop", "S6b parked on [stop] after the load");
    h.tick();
    // 修前：active_wait_icon_ 存活 → advance_click_wait 的隐藏路径对旧 id
    // set_props（缺失即物化）→ 幽灵层出现。
    check(!h.has_layer("waiticon"),
          "S6b no ghost click-wait icon node materialized after the load (缺-7)");
    check(h.rt->scene().size() == 1, "S6b scene is exactly the restored snapshot");
}

// ---------------------------------------------------------------------------
// S6c — 缺-8：skip 模式不得跨读档存活
// ---------------------------------------------------------------------------
void test_skip_mode_does_not_survive_load() {
    Harness h("[exec command=\"skip\" mode=\"1\"]\n[call file=\"story.iet\" label=\"main\"]\n");
    h.warm(3);
    check(h.rt->skip_active(), "S6c setup: command skip is active");
    auto d = scene_fixture({lay("s", props({{"file", "s.png"}}))}, kLineStop);
    h.install("s6c.dat", d);
    check(h.rt->load_game("s6c.dat", 0) == oa::runtime::LoadResult::Ok, "S6c load Ok");
    h.tick();
    check(!h.rt->skip_active(), "S6c skip mode does not survive a load (缺-8)");
    check(!h.rt->control_skip_effective(), "S6c control-skip latch also cleared");
}

// ---------------------------------------------------------------------------
// S6d — 缺-9：script_status==4 不得让读档后的剧本停摆
// ---------------------------------------------------------------------------
void test_script_status_reset_on_load() {
    Harness h;
    h.run_lua("function oa_ss(e, p) e:setScriptStatus(p.n) end\n");
    h.warm(3);
    check(h.wait_str() == "wt0", "S6d setup: parked on the click wait ([wt0])");

    // 控制臂：status=4 时点击**不**推进（闸是真的、测试有意义）。
    check(h.rt->interpreter().lua_bridge().call_function("oa_ss", {{"n", "4"}}),
          "S6d status set to 4 via the engine handle");
    const size_t before = h.pos().second;
    h.tick(-5, -5, true);
    check(h.pos().second == before && h.wait_str() == "wt0",
          "S6d control: status==4 parks the story (a click does not advance)");

    // 读档：停驻行仍是 [wt]（点击停驻）；status 被读档清 0 ⇒ 点击能推进。
    auto d = scene_fixture({lay("s", props({{"file", "s.png"}}))}, kLineClickWait);
    h.install("s6d.dat", d);
    check(h.rt->load_game("s6d.dat", 0) == oa::runtime::LoadResult::Ok, "S6d load Ok");
    h.tick();
    check(h.wait_str() == "wt0", "S6d re-parked on the click wait after the load");
    h.tick(-5, -5, true); // 点击释放停驻（advance_wait 每 tick 走一步）
    check(h.wait_str() != "wt0" && h.pos().second > kLineClickWait,
          "S6d the click releases the wait (script_status cleared by the load; 缺-9)");
    h.tick();
    check(h.wait_str() == "stop",
          "S6d the story keeps running after the load (缺-9: no STOP_NO_INPUT hang)");
}

// ---------------------------------------------------------------------------
// S7 — ② 桶特判保留：interpreter tag 队列跨读档存活
// ---------------------------------------------------------------------------
void test_tag_queue_preserved_across_load() {
    Harness h;
    h.warm();
    // 触发读档的那一步自己排的 tag（FPM 的 estag 链同形态）必须在读档后执行。
    h.rt->interpreter().enqueue_tag("lyc", {{"id", "from_queue"}, {"file", "bg.png"}});
    auto d = scene_fixture({lay("base", props({{"file", "base.png"}}))}, kLineStop);
    h.install("s7.dat", d);
    check(h.rt->load_game("s7.dat", 0) == oa::runtime::LoadResult::Ok, "S7 load Ok");
    check(h.has_layer("from_queue"),
          "S7 a tag queued before the load runs against the restored scene (缺-5 = ② 保留)");
    check(h.has_layer("base"), "S7 the restored scene is intact");
}

// ---------------------------------------------------------------------------
// S8 — 失败原子性：文件缺失 / 损坏 / 位置预检失败 ⇒ 状态不变
// ---------------------------------------------------------------------------
void test_failed_load_is_atomic() {
    Harness h;
    h.warm();
    const auto pos_before = h.pos();
    const std::string wait_before = h.wait_str();
    const size_t layers_before = h.rt->scene().size();
    check(h.has_layer("pre"), "S8 setup: boot scene present");

    // (a) 文件缺失
    check(h.rt->load_game("nope.dat", 0) == oa::runtime::LoadResult::MissingFile,
          "S8a missing file -> LoadResult::MissingFile");
    check(h.rt->load_game_from("nope.dat", 0) == false,
          "S8a the bool projection stays false");
    // (b) 损坏（合法头 + 截断载荷）
    const std::string doc =
        scene_fixture({lay("x", props({{"file", "x.png"}}))}, kLineStop).encode();
    h.store->files["save/bad.dat"] = std::vector<uint8_t>(doc.begin(), doc.begin() + 8);
    check(h.rt->load_game("bad.dat", 0) == oa::runtime::LoadResult::Corrupt,
          "S8b truncated payload -> LoadResult::Corrupt");
    // (c) 位置预检失败（脚本不存在）——旧实现会在这里半恢复（场景已清、快照已灌）
    auto d = scene_fixture({lay("ghost_of_failed_load", props({{"file", "g.png"}}))}, kLineStop);
    d.current_script = "missing.iet";
    h.install("s8c.dat", d);
    check(h.rt->load_game("s8c.dat", 0) == oa::runtime::LoadResult::PositionUnavailable,
          "S8c unavailable script -> LoadResult::PositionUnavailable");

    // 三次失败之后：运行时状态逐项不变。
    check(h.rt->scene().size() == layers_before, "S8 scene size unchanged after failures");
    check(h.has_layer("pre") && !h.has_layer("ghost_of_failed_load"),
          "S8 no snapshot layer leaked from a failed load (no half-restore)");
    check(h.pos() == pos_before, "S8 interpreter position unchanged");
    check(h.wait_str() == wait_before, "S8 wait state unchanged");
    check(h.rt->interpreter().variables().get("fx") == std::nullopt,
          "S8 local variables from the failed fixtures were not injected");
}

// ---------------------------------------------------------------------------
// S9 — 读档派发的事件不丢（迭代器安全化）
// ---------------------------------------------------------------------------
void test_events_emitted_during_load_dispatch_survive() {
    // story 第 0 行发 [load] ⇒ 读档走 tick 内的 dispatch_save_events 派发循环；
    // onLoad 钩子（Lua restore()，与 FPM 同形）排一个 tag，该 tag 在**读档链自己
    // 的排空**里执行并产生一个 Trans 事件 —— 它必须活下来（旧实现在原地遍历被
    // push_back 的队列，尾部 swap 会把这个事件整批丢掉）。
    Harness h("[call file=\"story.iet\" label=\"main\"]\n",
              "*main\n[load file=\"s9.dat\"]\n[stop]\n");
    h.run_lua("function restore(e, p) e:tag{\"trans\", type=\"0\"} end\n");
    auto d = scene_fixture({lay("s9scene", props({{"file", "s.png"}}))}, 1 /* [stop] */);
    h.install("s9.dat", d);
    h.tick();
    check(h.has_layer("s9scene"), "S9 the scripted [load] performed the load");
    bool saw_trans = false;
    for (const auto& e : h.rt->drain_events())
        if (e.kind == oa::runtime::Event::Kind::Trans) saw_trans = true;
    check(saw_trans,
          "S9 an event produced by the load chain itself (Lua restore() -> e:tag) "
          "reaches the host pump");
}

} // namespace

int main() {
    test_missing_parent_and_unresolvable_file();
    test_crop_group_geometry_semantics();
    test_empty_scene_and_local_only();
    test_malformed_and_duplicate_ids();
    test_legacy_reserved_prefix_record_only();
    test_inflight_alldelete_cancelled_by_load();
    test_no_ghost_click_wait_icon();
    test_skip_mode_does_not_survive_load();
    test_script_status_reset_on_load();
    test_tag_queue_preserved_across_load();
    test_failed_load_is_atomic();
    test_events_emitted_during_load_dispatch_survive();
    if (failures == 0) {
        std::printf("save_compat_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "save_compat_test: %d check(s) failed\n", failures);
    return 1;
}
