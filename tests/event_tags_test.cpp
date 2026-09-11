// M5b: native tag events — layer/text/message/config tags produce typed
// events with string-preserving params; truly unknown tags stay Custom.
#include <cstdio>
#include <string>
#include <vector>

#include "core/runtime/runtime_iet.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
using oa::runtime::CallbackResult;
using oa::runtime::Event;
using oa::runtime::ExecutionResult;
using oa::runtime::Interpreter;

struct Collector {
    std::vector<Event> events;
    CallbackResult cb(const Event& e) {
        events.push_back(e);
        return e.kind == Event::Kind::Wait_ ? CallbackResult::Pause
                                            : CallbackResult::Continue;
    }
};

void run_all(Interpreter& it, Collector& c) {
    it.set_callback([&c](const Event& e) { return c.cb(e); });
    it.start("test", "main");
    for (;;) {
        const ExecutionResult r = it.run();
        if (r == ExecutionResult::Completed) break;
        it.next_line();
    }
}

size_t count_kind(const Collector& c, Event::Kind k) {
    size_t n = 0;
    for (const auto& e : c.events)
        if (e.kind == k) ++n;
    return n;
}

const Event* find_kind(const Collector& c, Event::Kind k) {
    for (const auto& e : c.events)
        if (e.kind == k) return &e;
    return nullptr;
}
} // namespace

int main() {
    Interpreter it;
    Collector c;
    it.load_script("test", R"(
*main
[lyc id="1.0" file="bg001_a" path=":bg/"]
[lyprop id="1.0" visible="1" alpha="255"]
[lydel id="1"]
[print data="こんにちは"]
[rt]
[rp]
[chgmsg id="adv01.mw.adv"]
[/chgmsg]
[splay file="bgm01" loop="1"]
[mouse hide="1"]
[totally_unknown_tag x="1"]
[stop]
)");
    run_all(it, c);

    const Event* create = find_kind(c, Event::Kind::LayerCreate);
    check(create != nullptr, "LayerCreate emitted");
    if (create) {
        check(create->id == "1.0", "layer id preserved as string");
        check(create->params.at("file") == "bg001_a", "file param preserved");
        check(create->params.at("path") == ":bg/", "path param preserved");
    }
    const Event* props = find_kind(c, Event::Kind::LayerSetProps);
    check(props && props->params.at("visible") == "1" && props->params.at("alpha") == "255",
          "lyprop params preserved");
    const Event* del = find_kind(c, Event::Kind::LayerDelete);
    check(del && del->id == "1", "lydel id preserved");
    const Event* line = find_kind(c, Event::Kind::ScenarioLine);
    check(line && line->content == "こんにちは", "print content");
    check(count_kind(c, Event::Kind::LineBreak) == 1, "rt -> LineBreak");
    check(count_kind(c, Event::Kind::PageBreak) == 1, "rp -> PageBreak");
    check(count_kind(c, Event::Kind::MessageLayerSwitch) == 1, "chgmsg");
    check(count_kind(c, Event::Kind::MessageLayerPop) == 1, "/chgmsg");
    const Event* cfg = find_kind(c, Event::Kind::ConfigEvent);
    check(cfg != nullptr, "config event for splay");
    if (cfg) check(cfg->params.at("file") == "bgm01", "splay params kept");
    check(count_kind(c, Event::Kind::Custom) == 1, "only the unknown tag is Custom");
    check(it.variables().get("x").has_value() == false, "no stray state");
    std::printf("event_tags_test: kinds create=%zu props=%zu del=%zu line=%zu cfg=%zu custom=%zu\n",
                count_kind(c, Event::Kind::LayerCreate),
                count_kind(c, Event::Kind::LayerSetProps),
                count_kind(c, Event::Kind::LayerDelete),
                count_kind(c, Event::Kind::ScenarioLine),
                count_kind(c, Event::Kind::ConfigEvent),
                count_kind(c, Event::Kind::Custom));
    if (failures) {
        std::fprintf(stderr, "event_tags_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("event_tags_test: all ok\n");
    return 0;
}
