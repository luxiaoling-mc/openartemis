// Host input driver for tests: drive the runtime like the app does
// (mouse_move / mouse_button / key_event + tick(delta)), so every frame runs
// finish_frame() and queued events reach the renderer. Prefer a real click on a
// real layer over injected tags.
#pragma once

#include <functional>
#include <map>
#include <set>
#include <optional>
#include <string>

#include "core/render/text.h"
#include "core/runtime/runtime_game.h"

namespace oa::test {

/// One host frame without input.
inline void frame(oa::runtime::GameRuntime& rt) {
    rt.tick(16);
}

/// One host frame with the cursor at (mx,my).
inline void frame_at(oa::runtime::GameRuntime& rt, int mx, int my) {
    rt.mouse_move(float(mx), float(my));
    rt.tick(16);
}

/// Hover, press, release, settle.
inline void click_at(oa::runtime::GameRuntime& rt, int mx, int my) {
    rt.mouse_move(float(mx), float(my));
    rt.tick(16);
    rt.mouse_button(1, true);
    rt.tick(16);
    rt.mouse_button(1, false);
    rt.tick(16);
}

/// Replay one test-shaped input frame through the host path.
///
/// Frame state is applied before the tick (a release therefore lands in the
/// released frame, which drag-out dispatch expects). Held keys are tracked so a
/// key that disappears from keys_down is released by the host.
inline void replay(oa::runtime::GameRuntime& rt, const oa::runtime::FrameInput& in) {
    static std::map<const void*, std::set<int>> held;
    rt.mouse_move(float(in.mouse_x), float(in.mouse_y));

    std::set<int> want(in.keys_down.begin(), in.keys_down.end());
    want.insert(in.key_down_edges.begin(), in.key_down_edges.end());
    for (const int vk : in.key_up_edges) want.erase(vk);
    auto& h = held[&rt];
    for (auto it = h.begin(); it != h.end();) {
        if (want.count(*it)) {
            ++it;
            continue;
        }
        rt.key_event(*it, false);
        it = h.erase(it);
    }
    for (const int vk : want) {
        rt.key_event(vk, true);
        h.insert(vk);
    }

    rt.mouse_button(1, in.left_down || in.left_click_edge);
    if (in.right_click_edge) {
        rt.mouse_button(2, true);
        rt.mouse_button(2, false);
    }
    rt.tick(16);
}

/// Right click (back / menu).
inline void right_click(oa::runtime::GameRuntime& rt) {
    rt.mouse_button(2, true);
    rt.tick(16);
    rt.mouse_button(2, false);
    rt.tick(16);
}

/// Press/release a key.
inline void key(oa::runtime::GameRuntime& rt, int vk, bool down) {
    rt.key_event(vk, down);
    rt.tick(16);
}

/// Centre of the biggest layer handling `event` (the ADV click screen).
inline std::optional<std::pair<int, int>> screen_click_point(
    oa::runtime::GameRuntime& rt, const char* event = "click") {
    const oa::render::Layer* best = nullptr;
    double best_area = 0;
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, event);
        if (!h || !l->visible) continue;
        const double w = l->has_clip ? l->clip_w : (l->width > 0 ? l->width : 0);
        const double hh = l->has_clip ? l->clip_h : (l->height > 0 ? l->height : 0);
        if (w <= 0 || hh <= 0) continue;
        if (w * hh > best_area) {
            best = l;
            best_area = w * hh;
        }
    }
    if (!best) return std::nullopt;
    const double w = best->has_clip ? best->clip_w : best->width;
    const double hh = best->has_clip ? best->clip_h : best->height;
    double x0 = 0, y0 = 0, rw = 0, rh = 0;
    if (!rt.scene().world_rect(best->id, w, hh, &x0, &y0, &rw, &rh)) return std::nullopt;
    return std::make_pair(int(x0 + rw / 2), int(y0 + rh / 2));
}

/// Centre of a click row by key (bt_start, bt_conf, …).
inline std::optional<std::pair<int, int>> row_click_point(oa::runtime::GameRuntime& rt,
                                                          const std::string& key,
                                                          const char* event = "click") {
    for (const oa::render::Layer* l : rt.scene().draw_order()) {
        const auto* h = rt.scene().find_event_handler(l->id, event);
        if (!h || !l->visible) continue;
        bool hit = false;
        for (const char* field : {"key", "name", "function"}) {
            const auto it = h->params.find(field);
            if (it != h->params.end() && it->second == key) hit = true;
        }
        if (!hit) continue;
        const double w = l->has_clip ? l->clip_w : (l->width > 0 ? l->width : 180.0);
        const double hh = l->has_clip ? l->clip_h : (l->height > 0 ? l->height : 80.0);
        double x0 = 0, y0 = 0, rw = 0, rh = 0;
        if (!rt.scene().world_rect(l->id, w, hh, &x0, &y0, &rw, &rh)) continue;
        return std::make_pair(int(x0 + rw / 2), int(y0 + rh / 2));
    }
    return std::nullopt;
}

/// Story message layer (largest text layer, name layers excluded).
inline const oa::render::MessageLayer* story_layer(oa::runtime::GameRuntime& rt) {
    const oa::render::MessageLayer* found = nullptr;
    for (const std::string& id : rt.text().visible_content_layers()) {
        const oa::render::MessageLayer* ml = rt.text().layer(id);
        if (!ml || ml->char_count == 0) continue;
        if (ml->id.find("name") != std::string::npos) continue;
        if (!found || ml->char_count > found->char_count) found = ml;
    }
    return found;
}

/// Page text sample.
inline std::string page_text(const oa::render::MessageLayer* ml, size_t cap = 24) {
    if (!ml) return {};
    std::string s;
    for (const auto& u : ml->page) {
        if (u.data.empty()) continue;
        s += u.data;
        if (s.size() > cap) break;
    }
    return s;
}

/// Tick until `pred` holds; returns frames used or -1 on timeout.
inline int wait_for(oa::runtime::GameRuntime& rt, int max_frames,
                    const std::function<bool()>& pred) {
    for (int f = 0; f < max_frames && !rt.exit_requested(); ++f) {
        if (pred()) return f;
        rt.tick(16);
    }
    return pred() ? max_frames : -1;
}

/// Title -> click start -> first story page.
inline bool start_story_from_title(oa::runtime::GameRuntime& rt, int title_budget = 4000,
                                   int story_budget = 3000) {
    if (story_layer(rt)) return true;

    for (int f = 0; f < title_budget && !rt.exit_requested(); ++f) {
        if (const auto p = row_click_point(rt, "bt_start")) {
            click_at(rt, p->first, p->second);
            break;
        }
        rt.tick(16);
    }
    for (int f = 0; f < story_budget && !rt.exit_requested(); ++f) {
        rt.tick(16);
        const auto* ml = story_layer(rt);
        if (ml && ml->char_count > 0) return true;
        // park without text (dialogs, transitions): keep clicking through
        if (f % 40 == 0) {
            if (const auto p = screen_click_point(rt)) click_at(rt, p->first, p->second);
        }
    }
    return false;
}


/// Hold a key for `frames` then release (what a player does; a single-frame
/// edge is what the data-side push handlers treat as a glitch).
inline void hold_key(oa::runtime::GameRuntime& rt, int vk, int frames = 12, int settle = 4) {
    rt.key_event(vk, true);
    for (int f = 0; f < frames && !rt.exit_requested(); ++f) rt.tick(16);
    rt.key_event(vk, false);
    for (int f = 0; f < settle && !rt.exit_requested(); ++f) rt.tick(16);
}

/// Click the centre of a specific layer (by id). False when it is absent or
/// has no world rect.
inline bool click_layer(oa::runtime::GameRuntime& rt, const std::string& id) {
    const oa::render::Layer* l = rt.scene().find(id);
    if (!l) return false;
    double w = l->has_clip ? l->clip_w : (l->width > 0 ? l->width : 60.0);
    double h = l->has_clip ? l->clip_h : (l->height > 0 ? l->height : 30.0);
    double x0 = 0, y0 = 0, rw = 0, rh = 0;
    if (!rt.scene().world_rect(id, w, h, &x0, &y0, &rw, &rh)) return false;
    click_at(rt, int(x0 + rw / 2), int(y0 + rh / 2));
    return true;
}

/// A story page park: a click-advanceable wait.
inline bool is_page_wait(const oa::runtime::Wait* w) {
    return w && (w->waiting_click() ||
                 (w->timed() && w->can(oa::runtime::Wait::OnClick)));
}

/// True when the visible story page has finished its per-glyph reveal.
inline bool reveal_done(oa::runtime::GameRuntime& rt) {
    for (const std::string& id : rt.text().visible_content_layers()) {
        if (!rt.text().page_has_visible_text(id)) continue;
        const oa::render::MessageLayer* ml = rt.text().layer(id);
        // 揭示是否还在进行以 reveal_pending 为准（与引擎 is_reveal_complete 一致）；
        // 读档/换页后的层可能 idx<count 但已经不再推进。
        if (ml && ml->reveal_pending) return false;
    }
    return true;
}

/// One click on the ADV screen that actually turns the page: waits for the
/// reveal first, then waits until the text changes. Returns false when the page
/// did not change (a [stop] park, a choice, a UI screen).
inline bool advance_page(oa::runtime::GameRuntime& rt, int budget = 1200) {
    const auto* before = story_layer(rt);
    const std::string was = page_text(before);
    if (!wait_for(rt, 400, [&] { return reveal_done(rt); }) && !reveal_done(rt))
        return false;
    if (const auto p = screen_click_point(rt)) click_at(rt, p->first, p->second);
    const int used = wait_for(rt, budget, [&] {
        const auto* now = story_layer(rt);
        return now && page_text(now) != was;
    });
    return used >= 0;
}

/// Get back to a clickable story page: leave any UI screen (right click) and
/// turn pages until text is on screen and settled.
inline bool ensure_story(oa::runtime::GameRuntime& rt, int budget = 4000) {
    for (int f = 0; f < budget && !rt.exit_requested(); ++f) {
        const auto* ml = story_layer(rt);
        const auto* w = rt.current_wait();
        if (ml && ml->char_count > 0 && w && w->waiting_click() && reveal_done(rt))
            return true;
        if (f % 60 == 0) {
            if (!ml) right_click(rt);
            else if (const auto p = screen_click_point(rt)) click_at(rt, p->first, p->second);
        }
        rt.tick(16);
    }
    return false;
}

} // namespace oa::test
