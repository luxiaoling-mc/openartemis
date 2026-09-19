// E-mote stencil/mask structure survey (report-only, not a ctest).
// The engine currently ignores type-12 stencil-composite nodes (the eyes'
// 眼珠 clipping) — this survey walks a real PSB's raw node objects and
// dumps every field the files actually carry on/around type-12 nodes, so
// the mask semantics get implemented against the real data layout instead
// of guesses.
//
// usage: emote_mask_dump <root.pfs> <psb-name-substring>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/fs/physfs_fs.h"
#include "core/emote/psb_reader.h"
#include "core/emote/emote_player.h"

namespace oa::emote {
// narrow internal helper re-declared: psb_reader.h exposes the reader; the
// object walking is public API, so the survey uses it directly.

static void dump_value(const PsbReader& psb, uint32_t off, int depth);

static void dump_node(const PsbReader& psb, uint32_t node_off,
                      std::string& path, int depth,
                      std::map<std::string, int>* type12_fields,
                      int* type12_count, int* node_count) {
    std::vector<std::string> keys;
    std::vector<uint32_t> vals;
    if (!psb.object_entries(node_off, &keys, &vals)) return;
    ++*node_count;

    // label + type
    std::string label;
    long long type = 0;
    bool is12 = false;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == "label") {
            if (auto s = psb.read_string(vals[i])) label = *s;
        } else if (keys[i] == "type") {
            psb.read_int(vals[i], &type);
        }
    }
    is12 = type == 12;
    if (is12) ++*type12_count;

    std::printf("[mask] node '%s%s' type=%lld\n",
                path.c_str(), label.empty() ? "?" : label.c_str(), type);
    if (is12) {
        std::printf("[mask]   keys:");
        for (const auto& k : keys) std::printf(" %s", k.c_str());
        std::printf("\n");
    }
    // icon reference of the current content (if any)
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] != "frameList") continue;
        std::vector<uint32_t> frames;
        if (!psb.list_items(vals[i], &frames)) continue;
        for (uint32_t fo : frames) {
            auto c = psb.object_member(fo, "content");
            if (!c || psb.kind_at(*c) != Kind::Objects) continue;
            auto src = psb.object_member(*c, "src");
            auto ic = psb.object_member(*c, "icon");
            std::string sv, iv;
            if (src) if (auto q = psb.read_string(*src)) sv = *q;
            if (ic) if (auto q = psb.read_string(*ic)) iv = *q;
            if (!sv.empty() || !iv.empty())
                std::printf("[mask]   frame src='%s' icon='%s'\n",
                            sv.c_str(), iv.c_str());
        }
    }
    for (size_t i = 0; i < keys.size(); ++i) {
        const std::string& k = keys[i];
        if (k == "frameList" || k == "children") continue;
        if (is12) {
            (*type12_fields)[k]++;
            const Kind kind = psb.kind_at(vals[i]);
            std::printf("[mask]   %s kind=0x%02x ", k.c_str(),
                        unsigned(kind));
            if (kind >= Kind::StringN1 && kind <= Kind::StringN4) {
                std::printf("= '%s'\n", psb.read_string(vals[i])->c_str());
            } else if ((kind >= Kind::ArrayN1 && kind <= Kind::ArrayN8) ||
                       kind == Kind::List) {
                std::vector<uint32_t> items;
                psb.list_items(vals[i], &items) ||
                    psb.array_values(vals[i], &items);
                std::printf("= list[%zu]:", items.size());
                for (size_t j = 0; j < items.size() && j < 8; ++j) {
                    const Kind ik = psb.kind_at(items[j]);
                    if (auto s = psb.read_string(items[j]))
                        std::printf(" '%s'", s->c_str());
                    else if (ik == Kind::Objects)
                        std::printf(" <obj>");
                    else if (ik >= Kind::NumberN0 && ik <= Kind::NumberN8) {
                        int64_t n = 0;
                        psb.read_int(items[j], &n);
                        std::printf(" %lld", (long long)n);
                    } else
                        std::printf(" <k%02x>", unsigned(ik));
                }
                std::printf("\n");
            } else {
                double d = 0;
                int64_t n = 0;
                if (psb.read_double(vals[i], &d)) std::printf("= %g\n", d);
                else if (psb.read_int(vals[i], &n)) std::printf("= %lld\n",
                                                                (long long)n);
                else if (kind == Kind::Objects) {
                    std::printf("= object:\n");
                    dump_value(psb, vals[i], depth + 1);
                } else std::printf("\n");
            }
        }
    }

    // recurse children with the path context
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] != "children") continue;
        std::vector<uint32_t> kids;
        if (!psb.list_items(vals[i], &kids)) continue;
        for (uint32_t kid : kids) {
            std::string child_path = path + label + "/";
            dump_node(psb, kid, child_path, depth + 1, type12_fields,
                      type12_count, node_count);
        }
    }
}

static void dump_value(const PsbReader& psb, uint32_t off, int depth) {
    std::vector<std::string> keys;
    std::vector<uint32_t> vals;
    if (!psb.object_entries(off, &keys, &vals)) return;
    for (size_t i = 0; i < keys.size(); ++i) {
        const Kind kind = psb.kind_at(vals[i]);
        std::printf("[mask]     %s kind=0x%02x", keys[i].c_str(),
                    unsigned(kind));
        if (kind >= Kind::StringN1 && kind <= Kind::StringN4) {
            std::printf("= '%s'\n", psb.read_string(vals[i])->c_str());
        } else {
            double d = 0;
            if (psb.read_double(vals[i], &d)) std::printf("= %g\n", d);
            else std::printf("\n");
        }
    }
    (void)depth;
}
} // namespace oa::emote

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::printf("usage: emote_mask_dump <root.pfs> <psb-substring>\n");
        return 2;
    }
    const char* pfs_path = argv[1];
    const std::string sub = argv[2];

    auto fs = std::make_shared<oa::fs::PhysFileSystem>(pfs_path);
    // locate the psb
    std::string psb;
    for (const std::string& dir : {std::string("image/fg"), std::string("movie")}) {
        auto items = fs->list(dir);
        if (!items) continue;
        for (const std::string& n : *items) {
            if (n.size() < 4 || n.substr(n.size() - 4) != ".psb") continue;
            if (n.find(sub) == std::string::npos) continue;
            psb = dir + "\\" + n;
            break;
        }
        if (!psb.empty()) break;
    }
    if (psb.empty()) {
        std::printf("[mask] no psb matching '%s'\n", sub.c_str());
        return 3;
    }
    auto bytes = fs->read(psb);
    if (!bytes) {
        std::printf("[mask] read failed: %s\n", psb.c_str());
        return 4;
    }
    std::printf("[mask] psb '%s' (%zu bytes)\n", psb.c_str(), bytes->size());

    // ---- render mode: stencil ON/OFF pixel evidence over the idle loop ----
    if (argc > 3 && std::string(argv[3]) == "render") {
        oa::emote::EmotePlayer player;
        std::string perr;
        if (!player.load(*bytes, 1920, 1620, &perr)) {
            std::printf("[mask] player load failed: %s\n", perr.c_str());
            return 7;
        }
        for (const auto& t : player.file().timelines)
            if (t.loopEnd > 0) player.fade_in_timeline(t.label);
        std::vector<oa::emote::EmoteDrawPart> parts;
        player.collect_pose_parts(&parts, &perr);
        struct GBox { double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300; };
        std::map<int, GBox> boxes;
        int n_mask_parts = 0;
        for (const auto& pr : parts) {
            if (pr.mask_role == 1) {
                ++n_mask_parts;
                continue;
            }
            if (pr.stencil_group < 0) continue;
            GBox& b = boxes[pr.stencil_group];
            for (const auto& v : pr.verts) {
                b.x0 = std::min(b.x0, v.x); b.x1 = std::max(b.x1, v.x);
                b.y0 = std::min(b.y0, v.y); b.y1 = std::max(b.y1, v.y);
            }
        }
        std::printf("[mask] stencil groups with content: %zu (mask parts: %d)\n",
                    boxes.size(), n_mask_parts);
        const int W = player.width(), H = player.height();
        auto diff_px = [&](const std::vector<uint8_t>& a,
                            const std::vector<uint8_t>& b, const GBox& bx) {
            long n = 0;
            for (int y = std::max(0, int(bx.y0)); y <= std::min(H - 1, int(bx.y1)); ++y)
                for (int x = std::max(0, int(bx.x0)); x <= std::min(W - 1, int(bx.x1)); ++x) {
                    const size_t p4 = (size_t(y) * W + x) * 4;
                    for (int c = 0; c < 4; ++c)
                        if (std::abs(int(a[p4 + c]) - int(b[p4 + c])) > 8) {
                            ++n;
                            break;
                        }
                }
            return n;
        };
        std::map<int, long> gap_by_group;
        uint64_t last_rev = 0;
        std::map<int, std::string> gap_tl_by_group;
        if (boxes.empty()) {
            std::printf("[mask] no stencil content\n");
            return 0;
        }
        // sweep EVERY variable over its range; report the max ON/OFF
        // pixel difference inside each stencil group's content bbox
        for (const auto& [gid, box] : boxes)
            std::printf("[mask] group %d content bbox=(%.0f,%.0f)-(%.0f,%.0f)\n",
                        gid, box.x0, box.y0, box.x1, box.y1);
        for (const auto& t : player.file().timelines)
            std::printf("[mask] tl '%s' diff=%d loop=[%d..%d] last=%d vars=%zu\n",
                        t.label.c_str(), t.diff, t.loopBegin, t.loopEnd,
                        t.lastTime, t.variables.size());
        // sweep every one-shot timeline: blink/close timelines open the
        // ON/OFF pixel difference inside the eye regions
        for (const auto& t : player.file().timelines) {
            if (t.loopEnd > 0 || t.variables.empty()) continue;
            player.play_timeline(t.label);
            for (int i = 0; i < 30; ++i) {
                player.advance_ms(16);
                const uint64_t rev = player.revision();
                if (rev == last_rev) continue;
                last_rev = rev;
                player.set_external_pose(false);
                oa::emote::emote_set_stencil_enabled(1);
                player.render_now();
                const std::vector<uint8_t> on_px = player.rgba();
                oa::emote::emote_set_stencil_enabled(0);
                player.render_now();
                const std::vector<uint8_t> off_px = player.rgba();
                oa::emote::emote_set_stencil_enabled(1);
                for (const auto& [g2, box2] : boxes) {
                    const long d = diff_px(on_px, off_px, box2);
                    if (d > gap_by_group[g2]) {
                        gap_by_group[g2] = d;
                        gap_tl_by_group[g2] = t.label;
                    }
                }
            }
            player.pass();
        }
        std::set<std::string> seen_vars;
        for (const auto& m : player.file().motions)
            for (const auto& pv : m.parameter) {
                if (pv.rangeEnd <= pv.rangeBegin) continue;
                for (double v : {pv.rangeBegin, (pv.rangeBegin + pv.rangeEnd) / 2.0, pv.rangeEnd}) {
                    if (!seen_vars.insert(pv.id + '=' + std::to_string(v)).second) continue;
                    player.set_variable(pv.id, v);
                    player.set_external_pose(false);
                    oa::emote::emote_set_stencil_enabled(1);
                    player.render_now();
                    const std::vector<uint8_t> on_px = player.rgba();
                    oa::emote::emote_set_stencil_enabled(0);
                    player.render_now();
                    const std::vector<uint8_t> off_px = player.rgba();
                    oa::emote::emote_set_stencil_enabled(1);
                    long d = 0, gid = -1;
                    for (const auto& [g2, box2] : boxes) {
                        const long dd = diff_px(on_px, off_px, box2);
                        if (dd > d) { d = dd; gid = g2; }
                    }
                    if (d > 0)
                        std::printf("[mask] var %s=%g -> stencil diff %ld px (group %ld)\n",
                                    pv.id.c_str(), v, d, gid);
                }
                player.set_variable(pv.id, 0);
            }
        for (const auto& [g2, g] : gap_by_group)
            std::printf("[mask] group %d max stencil pixel-diff=%ld px on '%s'\n",
                        g2, g, gap_tl_by_group[g2].c_str());
        std::printf("[mask] render mode done\n");
        return 0;
    }

    oa::emote::PsbReader psb_reader;
    std::string err;
    if (!psb_reader.load(bytes->data(), bytes->size(), &err)) {
        std::printf("[mask] psb load failed: %s\n", err.c_str());
        return 5;
    }
    using namespace oa::emote;
    const uint32_t root = psb_reader.root_offset();
    auto obj_off = psb_reader.object_member(root, "object");
    if (!obj_off) {
        std::printf("[mask] no object member\n");
        return 6;
    }
    std::vector<std::string> obj_keys;
    std::vector<uint32_t> obj_vals;
    psb_reader.object_entries(*obj_off, &obj_keys, &obj_vals);
    std::map<std::string, int> type12_fields;
    int type12_count = 0, node_count = 0;
    for (size_t i = 0; i < obj_keys.size(); ++i) {
        auto motion_off = psb_reader.object_member(obj_vals[i], "motion");
        if (!motion_off) continue;
        std::vector<std::string> mkeys;
        std::vector<uint32_t> mvals;
        psb_reader.object_entries(*motion_off, &mkeys, &mvals);
        for (size_t j = 0; j < mkeys.size(); ++j) {
            auto layer_off = psb_reader.object_member(mvals[j], "layer");
            if (!layer_off) continue;
            std::vector<uint32_t> layers;
            if (!psb_reader.list_items(*layer_off, &layers)) continue;
            std::string path = obj_keys[i] + "/" + mkeys[j] + "/";
            for (uint32_t lo : layers)
                dump_node(psb_reader, lo, path, 0, &type12_fields,
                          &type12_count, &node_count);
        }
    }
    std::printf("[mask] nodes=%d type12=%d\n", node_count, type12_count);
    std::printf("[mask] type12 field census:");
    for (const auto& [k, n] : type12_fields)
        std::printf(" %s(x%d)", k.c_str(), n);
    std::printf("\n");
    return 0;
}
