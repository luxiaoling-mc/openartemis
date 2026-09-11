// P2b render/animation tests: lyc mask field routing (R7), [lytween sync=1]
// runtime wait + handler-tag dispatch (R8-R10), P1b typed-property tweens
// through the runtime (R12).
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/fs/fs.h"
#include "core/runtime/runtime.h"
#include "core/render/layer.h"
// research/111: Compositor::advance_tweens/apply_lytweendel 按值返回
// std::vector<TweenDone>，其元素类型定义在 render 内部头（公开头只前向声明）。
#include "core/render/render_internal.h"
#include "core/runtime/runtime_iet.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
bool approx(double a, double b, double eps = 1e-6) {
    const double d = a - b;
    return d > -eps && d < eps;
}

using oa::render::Compositor;
using oa::render::Layer;

std::map<std::string, std::string> tag(const std::initializer_list<std::pair<const char*, const char*>>& kv) {
    std::map<std::string, std::string> m;
    for (const auto& [k, v] : kv) m[k] = v;
    return m;
}

// ---------------------------------------------------------------- R7 mask routing
void test_mask_routing_and_sync_query() {
    Compositor c;
    c.create("1", tag({{"file", "fg"}}));
    c.set_props("1", tag({{"mask", "fgmask"}}));
    check(c.find("1")->mask == "fgmask", "R7 mask stored on the layer");
    c.set_props("1", tag({{"mask", ""}}));
    check(c.find("1")->mask.empty(), "R7 empty mask clears the layer mask");
    // layer_tweens_finished: layer gone or no active tween on it
    c.create("2", {});
    std::map<std::string, std::string> p = {{"id", "2"}, {"param", "alpha"},
                                            {"from", "0"}, {"to", "255"},
                                            {"time", "100"}};
    c.apply_lytween(p, 1000);
    check(!c.layer_tweens_finished("2"), "R7 layer with a running tween not finished");
    check(c.layer_tweens_finished("nope"), "R7 missing layer counts as finished");
    c.advance_tweens(1100);
    check(c.layer_tweens_finished("2"), "R7 layer with settled tween finished");
}

// ---------------------------------------------------------- R8-R10 sync + handlers
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

std::unique_ptr<oa::runtime::GameRuntime> make_runtime(MemFs& fs,
                                                       const std::string& script) {
    fs.files["system.ini"] =
        "[WINDOWS]\nWIDTH = 1280\nHEIGHT = 720\nFPS = 60\nCHARSET = UTF-8\n"
        "BOOT = system/first.iet\nSAVEPATH = save\n";
    fs.files["system/first.iet"] = script;
    auto rt = std::make_unique<oa::runtime::GameRuntime>(std::make_shared<MemFs>(fs));
    rt->open_project("windows");
    rt->boot_project();
    return rt;
}

void test_lytween_sync_wait() {
    // : a [lytween sync=1] parks the runtime in a
    // Stop{reason:"tween:<id>"} wait once the script completes without a wait
    // of its own, and the wait releases (WITHOUT advancing a line — the
    // interpreter already ran past the tag) when the layer's tweens finish.
    MemFs fs;
    auto rt = make_runtime(
        fs, "*main\n[lyc id=\"1\" file=\"a\"]\n"
            "[lytween id=\"1\" param=\"alpha\" from=\"0\" to=\"255\" time=\"120\" sync=\"1\"]\n");
    oa::runtime::FrameInput idle;
    rt->tick(16, idle);
    (void)rt->drain_events();
    const oa::runtime::WaitReason* w = rt->current_wait();
    check(w != nullptr, "R8 sync tween parks the script");
    check(w && w->kind == oa::runtime::WaitReason::Kind::Stop && w->id == "tween:1",
          "R8 sync wait is Stop{reason:tween:1}");
    // wait survives while the tween runs (clock advances in ticks)
    bool released = false;
    for (int i = 0; i < 20 && !released; ++i) {
        rt->tick(16, idle);
        (void)rt->drain_events();
        if (!rt->current_wait()) released = true;
    }
    check(released, "R8 sync wait released after the tween duration");
    // after release the layer settled at the final value (gc wrote it)
    const Layer* l = rt->scene().find("1");
    check(l && approx(l->alpha, 1.0), "R8 settled alpha after sync release");
}

void test_tween_handler_via_tag_queue() {
    // dispatch_tween_handlers -> enqueue_handler_tags (
    // ): a completed lytween with handler/calllua enqueues the
    // handler tag instead of calling Lua directly; the interpreter drains it
    // from its queue (also while parked under a non-Stop wait).
    MemFs fs;
    auto rt = make_runtime(
        fs, "*main\n[lyc id=\"1\" file=\"a\"]\n"
            "[lytween id=\"1\" param=\"alpha\" from=\"0\" to=\"255\" time=\"80\" "
            "handler=\"calllua\" function=\"notify_wait\" sys=\"1\"]\n[@]\n[stop]\n");
    oa::runtime::FrameInput idle;
    rt->tick(16, idle); // run: lyc + lytween + park on [@]
    (void)rt->drain_events();
    check(rt->current_wait() != nullptr, "R9 script parked on [@]");
    bool saw_queue = false;
    for (int i = 0; i < 8; ++i) {
        rt->tick(16, idle);
        (void)rt->drain_events();
        if (rt->interpreter().has_queued_tags()) saw_queue = true;
    }
    check(saw_queue, "R9 handler completion went through the interpreter tag queue");
    // parked-drain runs the queued calllua under the [@] wait (non-Stop gate)
    for (int i = 0; i < 4 && rt->interpreter().has_queued_tags(); ++i) {
        rt->tick(16, idle);
        (void)rt->drain_events();
    }
    check(!rt->interpreter().has_queued_tags(), "R9 queued handler drained while parked");
    // still parked on [@] (the handler only ran Lua; no position change)
    check(rt->current_wait() != nullptr, "R9 still parked after handler drain");
}

// ---------------------------------------------------------------- R12 typed props
void test_runtime_typed_property_tweens() {
    // P1b property tween coverage through the runtime event path: param values
    // reach the typed transform fields  and drive the layer.
    MemFs fs;
    auto rt = make_runtime(
        fs, "*main\n"
            "[lyc id=\"1\" file=\"a\"]\n"
            "[lytween id=\"1\" param=\"xscale\" from=\"100\" to=\"200\" time=\"100\"]\n"
            "[lytween id=\"1\" param=\"yscale\" from=\"100\" to=\"50\" time=\"100\"]\n"
            "[lytween id=\"1\" param=\"rotate\" from=\"0\" to=\"90\" time=\"100\"]\n"
            "[lytween id=\"1\" param=\"anchorx\" from=\"0\" to=\"10\" time=\"100\"]\n"
            "[lytween id=\"1\" param=\"anchory\" from=\"0\" to=\"20\" time=\"100\"]\n"
            "[wt 500 input=\"0\"]\n");
    oa::runtime::FrameInput idle;
    rt->tick(16, idle); // runs all tweens + parks on [wt]
    (void)rt->drain_events();
    const Layer* l = rt->scene().find("1");
    check(l && approx(l->x_scale, 100.0) && approx(l->y_scale, 100.0),
          "R12 typed props at their from value right after start");
    check(l && approx(l->rotate_deg, 0.0) && approx(l->anchor_x, 0.0) &&
              approx(l->anchor_y, 0.0),
          "R12 rotate/anchor at from");
    for (int i = 0; i < 8; ++i) rt->tick(16, idle); // past 100ms midpoint range
    (void)rt->drain_events();
    l = rt->scene().find("1");
    // 128ms elapsed > 100: tween finished -> settled at to (values written
    // through set_props typed pass,  gc_finished_tweens)
    check(l && approx(l->x_scale, 200.0, 1e-9), "R12 xscale settled to 200");
    check(l && approx(l->y_scale, 50.0, 1e-9), "R12 yscale settled to 50");
    check(l && approx(l->rotate_deg, 90.0, 1e-9), "R12 rotate settled to 90");
    check(l && approx(l->anchor_x, 10.0, 1e-9) && approx(l->anchor_y, 20.0, 1e-9),
          "R12 anchor settled");
}

} // namespace

int main() {
    test_mask_routing_and_sync_query();
    test_lytween_sync_wait();
    test_tween_handler_via_tag_queue();
    test_runtime_typed_property_tweens();
    if (failures) {
        std::fprintf(stderr, "render2_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("render2_test: all ok\n");
    return 0;
}
