// E9 diagnostic probe (not a pass/fail regression, not registered in ctest):
// dumps the parameterized motion axes that drive the idle loop's visual axes
// (head_slant/body_slant/head_UD/body_UD/head_LR/body_LR/act_sp) with node
// paths and per-key geometry, sweeps each axis against the neutral pose, and
// traces the idle entry trajectory of fixed head/body landmark icons at the
// game view.
// Env: OA_TEST_NEKOMIKO_PFS.
// Usage: emote_axis_dump <tay|tka> keys|sweep|one|idle|mesh
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/fs/physfs_fs.h"
#include "core/emote/emote_player.h"

using namespace oa::emote;

namespace {
const char* kAxis[] = {"head_slant", "body_slant", "head_UD", "body_UD",
                       "head_LR",    "body_LR",    "act_sp",  "move_UD"};

std::array<double, 4> bboxOf(const EmoteDrawPart& pr) {
    double mnX = 1e300, mxX = -1e300, mnY = 1e300, mxY = -1e300;
    for (const auto& v : pr.verts) {
        mnX = std::min(mnX, v.x); mxX = std::max(mxX, v.x);
        mnY = std::min(mnY, v.y); mxY = std::max(mxY, v.y);
    }
    return {mnX, mnY, mxX, mxY};
}

oa::emote::StaticRenderOptions game_view() {
    oa::emote::StaticRenderOptions opt;
    opt.fitToCanvas = false;
    opt.scale = 0.6;
    opt.dx = 960.0;                                        // reqW / 2
    opt.dy = 1620.0 / 2.0 + 3695.0 * 0.6 - 810.0 + 100.0; // setCoord y -> 2317
    return opt;
}

// recursive walk of a motion node tree (mirrors eval: sub-motion content
// dives into the referenced motion; children walk in the same motion).
void walk(const EmoteFile& f, const EmoteMotion& m, int nodeIdx,
          std::vector<std::string>& path, std::set<std::string>& printed,
          int depth) {
    if (depth > 128 || nodeIdx < 0 || nodeIdx >= int(m.nodes.size())) return;
    const EmoteNode& node = m.nodes[nodeIdx];
    if (node.removed || node.frames.empty()) return;
    path.push_back(node.label);
    std::string parId;
    if (node.isParameterized && node.parameterIndex >= 0 &&
        node.parameterIndex < int(m.parameter.size()))
        parId = m.parameter[size_t(node.parameterIndex)].id;
    const bool interesting = node.isParameterized &&
                             std::find(kAxis, kAxis + 8, parId) != kAxis + 8;
    if (interesting) {
        std::string p;
        for (size_t i = 0; i < path.size(); ++i) p += (i ? "/" : "") + path[i];
        const std::string key = p + "|" + parId;
        if (printed.insert(key).second) {
            double rb = 0, re = 1, div = 0;
            for (const auto& pa : m.parameter)
                if (pa.id == parId) { rb = pa.rangeBegin; re = pa.rangeEnd; div = pa.division; }
            std::printf("[axis] %s par=%s range=[%.0f..%.0f] div=%.0f keys=%zu\n",
                        p.c_str(), parId.c_str(), rb, re, div, node.frames.size());
            for (const auto& fr : node.frames) {
                std::string kind, ref;
                if (fr.hasContent) {
                    if (fr.isSubMotion) { kind = "sub"; ref = fr.subObject + "/" + fr.subMotion; }
                    else if (!fr.src.empty()) {
                        kind = fr.hasBp ? (fr.src == "blank" ? "morph" : "icon+grid")
                                        : (fr.src == "blank" ? "blank" : "icon");
                        ref = fr.src + "/" + fr.icon;
                    } else kind = "carrier";
                }
                std::string tail;
                if (fr.hasCoord)
                    tail += " coord=(" + std::to_string(int(fr.coordX)) + "," +
                            std::to_string(int(fr.coordY)) + ")";
                if (fr.hasAngle) tail += " ang=" + std::to_string(fr.angle);
                if (fr.zx != 1 || fr.zy != 1)
                    tail += " sc=(" + std::to_string(fr.zx) + "," + std::to_string(fr.zy) + ")";
                if (fr.hasBp) {
                    double mn = 9, mx = -9;
                    for (int i = 0; i < 32; ++i) {
                        mn = std::min(mn, double(fr.bp[i]));
                        mx = std::max(mx, double(fr.bp[i]));
                    }
                    tail += " bp[" + std::to_string(mn).substr(0, 6) + ".." +
                            std::to_string(mx).substr(0, 6) + "]";
                }
                std::printf("   t=%4.0f typ=%d %s%s%s\n", fr.time, fr.type,
                            fr.hasContent ? kind.c_str() : "none",
                            ref.empty() ? "" : (" ref=" + ref).c_str(), tail.c_str());
            }
        }
    }
    bool dove = false;
    for (const auto& fr : node.frames) {
        if (!fr.hasContent || !fr.isSubMotion) continue;
        const int midx = f.find_motion(fr.subObject, fr.subMotion);
        if (midx >= 0) {
            dove = true;
            const auto& sm = f.motions[size_t(midx)];
            if (!sm.layer.empty())
                walk(f, sm, sm.layer[0], path, printed, depth + 1);
        }
        break;
    }
    if (!dove)
        for (int ch : node.children) walk(f, m, ch, path, printed, depth + 1);
    path.pop_back();
}


// timelines mode: dump every timeline: label, diff, loop bounds, lastTime
// and all non-empty tracks with their keyframes.
void dump_timelines(const EmoteFile& f) {
    for (const auto& tl : f.timelines) {
        std::printf("[tl] '%s' diff=%d lastTime=%d loopBegin=%d loopEnd=%d tracks=%zu\n",
                    tl.label.c_str(), tl.diff, tl.lastTime, tl.loopBegin, tl.loopEnd,
                    tl.variables.size());
        for (const auto& tv : tl.variables) {
            std::string keys;
            for (const auto& fr : tv.frames) {
                char b[96];
                std::snprintf(b, sizeof(b), "(%g,%d,%g%s)", fr.time, fr.type, fr.value,
                              fr.hasContent ? "" : "e");
                keys += b;
            }
            std::printf("    %-24s %s\n", tv.label.c_str(), keys.c_str());
        }
    }
}
void dump_keys(const EmoteFile& f) {
    std::set<std::string> printed;
    const int midx = f.find_motion(f.baseChara, f.baseMotion);
    if (midx < 0) { std::printf("base motion missing\n"); return; }
    std::vector<std::string> path;
    for (int root : f.motions[size_t(midx)].layer)
        walk(f, f.motions[size_t(midx)], root, path, printed, 0);
}

// sweep mode: per-icon bbox displacement vs the neutral pose, one axis at a
// time, at the game view. No timeline: pose = f(vars) only.
void dump_sweep(const EmoteFile& f, const char* tag) {
    std::string err;
    const oa::emote::StaticRenderOptions opt = game_view();
    auto partsFor = [&](const std::map<std::string, double>& vars,
                        std::vector<EmoteDrawPart>* out) {
        return emote_collect_parts(f, vars, 1920, 1620, opt, out, &err);
    };
    std::vector<EmoteDrawPart> neutral;
    if (!partsFor({}, &neutral)) { std::printf("neutral collect fail\n"); return; }
    struct Cand { std::string src; std::string icon; std::array<double, 4> bb; double area; };
    std::vector<Cand> cands;
    std::set<std::string> seen;
    for (const auto& pr : neutral) {
        const auto& s = *f.sources[size_t(pr.source)];
        const std::string nm = s.icons[size_t(pr.icon)].name;
        if (!seen.insert(s.name + "/" + nm).second) continue;
        const auto bb = bboxOf(pr);
        const double w = bb[2] - bb[0], h = bb[3] - bb[1];
        if (w < 8 || h < 8) continue;
        cands.push_back({s.name, nm, bb, w * h});
    }
    auto pickBand = [&](double y0, double y1, int n, std::vector<Cand>* out) {
        std::vector<Cand> in;
        for (const auto& c : cands) {
            const double cy = (c.bb[1] + c.bb[3]) / 2;
            if (cy >= y0 && cy < y1) in.push_back(c);
        }
        std::sort(in.begin(), in.end(),
                  [](const Cand& a, const Cand& b) { return a.area > b.area; });
        for (int i = 0; i < n && i < int(in.size()); ++i) out->push_back(in[i]);
    };
    // Bands are derived from the figure's own extent (top/middle/bottom third
    // of the neutral part centres) instead of absolute canvas rows: the
    // absolute rows were calibrated on NekoMiko's view mapping, so on any
    // package whose setCoord/setScale places the figure elsewhere they matched
    // no part at all and the sweep printed an empty landmark list.
    double cy_lo = 1e30, cy_hi = -1e30;
    for (const auto& c : cands) {
        const double cy = (c.bb[1] + c.bb[3]) / 2;
        cy_lo = std::min(cy_lo, cy);
        cy_hi = std::max(cy_hi, cy);
    }
    std::vector<Cand> picks;
    if (!cands.empty() && cy_hi > cy_lo) {
        const double a = cy_lo + (cy_hi - cy_lo) / 3.0;
        const double b = cy_lo + 2.0 * (cy_hi - cy_lo) / 3.0;
        pickBand(cy_lo, a, 6, &picks);
        pickBand(a, b, 4, &picks);
        pickBand(b, cy_hi + 1.0, 6, &picks);
    }
    std::printf("[sweep] %s neutral landmarks (band by y, size):\n", tag);
    for (const auto& c : picks)
        std::printf("   %-28s %-24s bbox=(%7.1f,%7.1f)-(%7.1f,%7.1f) cy=%7.1f\n",
                    c.src.c_str(), c.icon.c_str(), c.bb[0], c.bb[1], c.bb[2], c.bb[3],
                    (c.bb[1] + c.bb[3]) / 2);
    struct Key { int src; int icon; };
    std::vector<Key> keys;
    for (const auto& c : picks) {
        int si = -1;
        for (size_t i = 0; i < f.sources.size(); ++i)
            if (f.sources[i]->name == c.src) { si = int(i); break; }
        if (si < 0) { keys.push_back({-1, -1}); continue; }
        int ii = -1;
        for (size_t i = 0; i < f.sources[size_t(si)]->icons.size(); ++i)
            if (f.sources[size_t(si)]->icons[i].name == c.icon) { ii = int(i); break; }
        keys.push_back({si, ii});
    }
    static const char* const sweepAxes[] = {"head_slant", "body_slant", "head_UD",
                                            "body_UD",    "head_LR",    "body_LR"};
    std::vector<std::map<std::string, double>> poses;
    std::vector<std::string> labels;
    poses.push_back({});
    labels.push_back("neutral");
    for (const char* ax : sweepAxes)
        for (int v = -30; v <= 30; v += 6) {
            std::map<std::string, double> vars;
            vars[ax] = double(v);
            poses.push_back(vars);
            char b[64];
            std::snprintf(b, sizeof(b), "%s=%+d", ax, v);
            labels.push_back(b);
        }
    for (size_t pi = 0; pi < poses.size(); ++pi) {
        std::vector<EmoteDrawPart> parts;
        if (!partsFor(poses[pi], &parts)) continue;
        std::map<std::pair<int, int>, std::array<double, 4>> now;
        for (const auto& pr : parts)
            now[{pr.source, pr.icon}] = bboxOf(pr);
        std::printf("[sweep] %s\n", labels[pi].c_str());
        for (size_t ki = 0; ki < keys.size(); ++ki) {
            const auto& c = picks[ki];
            if (keys[ki].src < 0) continue;
            auto it = now.find({keys[ki].src, keys[ki].icon});
            if (it == now.end()) {
                std::printf("   %-28s %-12s ABSENT\n", c.src.c_str(), c.icon.c_str());
                continue;
            }
            const auto& bb = it->second;
            const double dcx = (bb[0] + bb[2]) / 2 - (c.bb[0] + c.bb[2]) / 2;
            const double dcy = (bb[1] + bb[3]) / 2 - (c.bb[1] + c.bb[3]) / 2;
            std::printf("   %-28s %-12s d=(%+8.2f,%+8.2f) w=%6.1f h=%6.1f\n",
                        c.src.c_str(), c.icon.c_str(), dcx, dcy, bb[2] - bb[0],
                        bb[3] - bb[1]);
        }
    }
}

// idle trajectory: fixed head/body landmark icons (largest in each band at
// the neutral pose); drives the player like the game (fadeIn 通常待機) and
// prints head/body displacement from the first sample over ~4.5 s.
void dump_idle_traj(const EmoteFile& f, const char* tag) {
    std::string err;
    const oa::emote::StaticRenderOptions opt = game_view();
    std::vector<EmoteDrawPart> neutral;
    if (!emote_collect_parts(f, {}, 1920, 1620, opt, &neutral, &err)) return;
    struct Cand { std::string src, icon; std::array<double, 4> bb; double area; };
    std::vector<Cand> cands;
    std::set<std::string> seen;
    for (const auto& pr : neutral) {
        const auto& s = *f.sources[size_t(pr.source)];
        const std::string nm = s.icons[size_t(pr.icon)].name;
        if (!seen.insert(s.name + "/" + nm).second) continue;
        const auto bb = bboxOf(pr);
        const double w = bb[2] - bb[0], h = bb[3] - bb[1];
        if (w < 8 || h < 8) continue;
        cands.push_back({s.name, nm, bb, w * h});
    }
    auto pick = [&](double y0, double y1) {
        Cand* best = nullptr;
        for (auto& c : cands) {
            const double cy = (c.bb[1] + c.bb[3]) / 2;
            if (cy >= y0 && cy < y1 && (!best || c.area > best->area)) best = &c;
        }
        return best ? *best : Cand{};
    };
    const Cand headC = pick(0, 560);
    const Cand bodyC = pick(900, 1550);
    std::printf("[idle] %s head %s/%s body %s/%s\n", tag, headC.src.c_str(),
                headC.icon.c_str(), bodyC.src.c_str(), bodyC.icon.c_str());
    const char* pfs = std::getenv("OA_TEST_NEKOMIKO_PFS");
    oa::fs::PhysFileSystem fs_phys(pfs, false);    oa::fs::IFileSystem& fs = fs_phys;
    const char* file = (std::string(tag) == "tay") ? "image\\fhd\\fg\\aya\\tay_0.psb"
                                                   : "image\\fhd\\fg\\kae\\tka_0.psb";
    auto b = fs.read(file);
    if (!b) return;
    EmotePlayer p;
    if (!p.load(*b, 1920, 1620, &err)) return;
    p.set_scale(0.6, 0, 0);
    p.set_coord(0, 3695.0 * 0.6 - 810.0 + 100.0);
    p.fade_in_timeline("通常待機");
    auto centre = [&](const std::vector<EmoteDrawPart>& parts, const std::string& srcn,
                      const std::string& icn, double* cx, double* cy) {
        for (const auto& pr : parts) {
            const auto& s = *f.sources[size_t(pr.source)];
            if (s.name != srcn) continue;
            if (s.icons[size_t(pr.icon)].name != icn) continue;
            const auto bb = bboxOf(pr);
            *cx = (bb[0] + bb[2]) / 2; *cy = (bb[1] + bb[3]) / 2;
            return true;
        }
        return false;
    };
    double first[4] = {0};
    for (int step = 0; step <= 90; ++step) {
        p.advance_ms(50);
        if (step % 4) continue; // 200 ms samples
        std::vector<EmoteDrawPart> parts;
        p.collect_pose_parts(&parts, &err);
        if (parts.empty()) continue;
        double hx = 0, hy = 0, bx = 0, by = 0;
        if (!centre(parts, headC.src, headC.icon, &hx, &hy)) continue;
        if (!centre(parts, bodyC.src, bodyC.icon, &bx, &by)) continue;
        if (step == 0) { first[0] = hx; first[1] = hy; first[2] = bx; first[3] = by; }
        std::printf("[idle] %s t=%5.2fs hs=%7.2f bs=%7.2f headD=(%+6.1f,%+6.1f) "
                    "bodyD=(%+6.1f,%+6.1f) gapD=(%+6.1f,%+6.1f)\n",
                    tag, step * 0.05, p.get_variable("head_slant"),
                    p.get_variable("body_slant"), hx - first[0], hy - first[1],
                    bx - first[2], by - first[3], (hx - bx) - (first[0] - first[2]),
                    (hy - by) - (first[1] - first[3]));
    }
}
// ---------------------------------------------------------------------------
// S4 mesh mode (research/123) + S5 hybrid default (research/124 §1):
// grid-subdivision invariants (research/121 G3), the trim reverse assertion
// (research/121 §3), and a hybrid-vs-strict-node-vs-adaptive fidelity A/B
// against a converged reference mesh.
//
// Every assertion here is deliberately shaped so that a future edit breaks it:
//  * vertex count == 6*div^2 with the uv lattice exactly {i/div} (an added
//    trim offset/scale or a stray extra subdivision row fails);
//  * div == the node-authorised value when the node carries one, and == the
//    pre-S4 adaptive value when it does not (the S5 hybrid rule) — the strict
//    krkr `<2 -> 8` fallback is asserted separately on its own arm, so a
//    regression that silently drops either half of the hybrid fails;
//  * div is independent of the variable snapshot;
//  * the atlas mapping equals the literal (left+u*width)/texW with no extra
//    term (a future "trim compensation" fails);
//  * no part is coarser (or less accurate) than it was before S4 under the
//    default policy, while every authored node keeps the S4 improvement.
// ---------------------------------------------------------------------------
int g_meshFail = 0;
void mcheck(bool cond, const std::string& what) {
    if (!cond) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_meshFail;
    }
}

// The forward mesh is the piecewise-linear surface over the div x div cell
// grid; evaluating the known cell/triangle barycentrically reproduces exactly
// what the raster draws there (same diagonal as the renderer: (p0,p1,p2) and
// (p1,p3,p2) in cell order). The affine quad path (div == 1) uses the other
// diagonal but is only taken when the map IS affine (both diagonals agree),
// so the metric skips div < 2.
bool mesh_sample(const EmoteDrawPart& p, double u, double v, double* x, double* y) {
    const int div = p.meshDivision;
    if (div < 2 || u < 0 || u > 1 || v < 0 || v > 1) return false;
    if (p.verts.size() != size_t(6) * size_t(div) * size_t(div)) return false;
    int gx = int(std::floor(u * div)), gy = int(std::floor(v * div));
    if (gx >= div) gx = div - 1;
    if (gy >= div) gy = div - 1;
    const double fu = u * div - gx, fv = v * div - gy;
    const size_t base = (size_t(gy) * div + gx) * 6;
    const EmotePartVertex& a = p.verts[base + 0]; // (gx,   gy  )
    const EmotePartVertex& b = p.verts[base + 1]; // (gx+1, gy  )
    const EmotePartVertex& c = p.verts[base + 2]; // (gx,   gy+1)
    const EmotePartVertex& d = p.verts[base + 4]; // (gx+1, gy+1)
    if (fu + fv <= 1.0) {
        *x = a.x + fu * (b.x - a.x) + fv * (c.x - a.x);
        *y = a.y + fu * (b.y - a.y) + fv * (c.y - a.y);
    } else {
        *x = d.x + (1.0 - fu) * (b.x - d.x) + (1.0 - fv) * (c.x - d.x);
        *y = d.y + (1.0 - fu) * (b.y - d.y) + (1.0 - fv) * (c.y - d.y);
    }
    return true;
}

int node_authorised_div(int authored) {
    return authored < 2 ? 8 : authored;
}

// The pre-S4 adaptive rule, re-implemented HERE (not called) so that a change
// to the engine's fallback formula fails this test instead of following it.
int adaptive_div_ref(double iconW, double iconH) {
    const double diag = std::sqrt(iconW * iconW + iconH * iconH);
    return std::clamp(int(std::lround(diag / 90.0)), 6, 18);
}

// S5 hybrid expectation (research/124 §1): authored value when the node has
// one, the pre-S4 adaptive grid when it does not.
int hybrid_div_ref(int authored, double iconW, double iconH) {
    return authored < 2 ? adaptive_div_ref(iconW, iconH) : authored;
}

void dump_mesh(const EmoteFile& f, const char* tag) {
    std::string err;
    const oa::emote::StaticRenderOptions opt = game_view(); // fitToCanvas=false
    // Poses: neutral + the six parameter axes at +-20 + two face axes. The
    // view mapping is fit-free, so every arm shares one affine mapping and the
    // geometric metric below is not contaminated by a fit change.
    std::vector<std::pair<std::string, std::map<std::string, double>>> poses;
    poses.push_back({"neutral", {}});
    static const char* const axes[] = {"head_slant", "body_slant", "head_UD",
                                       "body_UD",    "head_LR",    "body_LR"};
    for (const char* ax : axes)
        for (int v : {-20, 20}) {
            std::map<std::string, double> vars;
            vars[ax] = double(v);
            poses.push_back({std::string(ax) + (v < 0 ? "=-20" : "=+20"), vars});
        }
    poses.push_back({"face_talk=3", {{"face_talk", 3}}});
    poses.push_back({"face_eye_open=0", {{"face_eye_open", 0}}});

    auto collect = [&](const std::map<std::string, double>& vars,
                       std::vector<EmoteDrawPart>* out) {
        out->clear();
        return emote_collect_parts(f, vars, 1920, 1620, opt, out, &err);
    };

    // ---- arm 1: the S5 hybrid rule (DEFAULT) ------------------------------
    // S5 (research/124 §1) ruling: a node with an authored meshDivision uses
    // it; a node WITHOUT one falls back to the pre-S4 adaptive grid instead of
    // krkr's flat 8 (never coarser than pre-S4). The strict krkr arm is run
    // separately below so both halves stay asserted.
    emote_set_mesh_div_policy(kEmoteMeshDivNodeAdaptive);
    std::printf("[mesh] %s poses=%zu policy=node-adaptive (hybrid, default)\n", tag,
                poses.size());
    std::vector<std::vector<EmoteDrawPart>> byPose(poses.size());
    std::vector<int> partCount;
    for (size_t i = 0; i < poses.size(); ++i) {
        if (!collect(poses[i].second, &byPose[i])) {
            std::printf("  FAIL: collect failed (%s)\n", poses[i].first.c_str());
            ++g_meshFail;
            return;
        }
        partCount.push_back(int(byPose[i].size()));
    }
    const std::vector<EmoteDrawPart>& neutral = byPose[0];
    std::printf("[mesh] parts per pose (neutral=%zu):", neutral.size());
    for (size_t i = 0; i < partCount.size(); ++i) std::printf(" %d", partCount[i]);
    std::printf("\n");

    // ---- vertex count / lattice / div == node.meshDivision ---------------
    size_t warped = 0, quad = 0, latticeBad = 0, divBad = 0, fallback = 0;
    size_t fallbackNotAdaptive = 0, authoredNotNode = 0, fallbackTooCoarse = 0;
    std::map<int, int> divHist, aDivHist, fbDivHist;
    auto icon_size_of = [&](const EmoteDrawPart& p, double* w, double* h) {
        if (p.source < 0 || p.source >= int(f.sources.size())) return false;
        const EmoteSource& s = *f.sources[size_t(p.source)];
        if (p.icon < 0 || p.icon >= int(s.icons.size())) return false;
        *w = s.icons[size_t(p.icon)].width;
        *h = s.icons[size_t(p.icon)].height;
        return true;
    };
    for (const auto& p : neutral) {
        if (p.meshDivision < 2) {
            ++quad;
            mcheck(p.verts.size() == 6, "quad part verts == 6");
            continue;
        }
        ++warped;
        const int d = p.meshDivision;
        divHist[d]++;
        aDivHist[p.authoredMeshDivision]++;
        double iw = 0, ih = 0;
        icon_size_of(p, &iw, &ih);
        // The hybrid rule, re-implemented HERE (see hybrid_div_ref) so that a
        // future engine change to either half fails this test.
        const int want = hybrid_div_ref(p.authoredMeshDivision, iw, ih);
        if (d != want) ++divBad;
        if (p.authoredMeshDivision < 2) {
            ++fallback;
            fbDivHist[d]++;
            if (d != adaptive_div_ref(iw, ih)) ++fallbackNotAdaptive;
            // "never coarser than before S4" is the whole point of the hybrid:
            // the fallback must not drop back to the strict krkr 8 unless the
            // adaptive rule itself picks 8 or less for this icon.
            if (d < node_authorised_div(p.authoredMeshDivision) &&
                d < adaptive_div_ref(iw, ih))
                ++fallbackTooCoarse;
        } else if (d != node_authorised_div(p.authoredMeshDivision)) {
            ++authoredNotNode;
        }
        // the engine helper must agree with the test-local formula too
        if (emote_adaptive_mesh_division(iw, ih) != adaptive_div_ref(iw, ih)) ++divBad;
        if (d < 2) continue;
        if (p.verts.size() != size_t(6) * size_t(d) * size_t(d)) {
            ++latticeBad;
            continue;
        }
        // uv lattice must be exactly {i/d} x {j/d}, all (d+1)^2 nodes present
        std::vector<uint8_t> seen(size_t(d + 1) * size_t(d + 1), 0);
        size_t nodes = 0;
        bool exact = true;
        for (const auto& v : p.verts) {
            const double gx = v.u * d, gy = v.v * d;
            const int ix = int(std::lround(gx)), iy = int(std::lround(gy));
            if (std::fabs(gx - ix) > 1e-9 || std::fabs(gy - iy) > 1e-9 || ix < 0 ||
                ix > d || iy < 0 || iy > d) {
                exact = false;
                break;
            }
            uint8_t& s = seen[size_t(iy) * size_t(d + 1) + size_t(ix)];
            if (!s) { s = 1; ++nodes; }
        }
        if (!exact || nodes != size_t(d + 1) * size_t(d + 1)) ++latticeBad;
    }
    std::printf("[mesh] parts warped=%zu quad=%zu fallbackAdaptive=%zu divHist:", warped,
                quad, fallback);
    for (const auto& [d, n] : divHist) std::printf(" %d=%d", d, n);
    std::printf(" authoredDivHist:");
    for (const auto& [d, n] : aDivHist) std::printf(" %d=%d", d, n);
    std::printf(" fallbackDivHist:");
    for (const auto& [d, n] : fbDivHist) std::printf(" %d=%d", d, n);
    std::printf("\n");
    mcheck(warped > 0, "warped parts present (mesh path exercised)");
    mcheck(latticeBad == 0, "vertex count == 6*div^2 and uv lattice == {i/div}^2");
    std::printf("[mesh] policy mode=%d (0=node krkr rule, 1=adaptive, 2=fixed, "
                "3=node-adaptive hybrid)\n", emote_mesh_div_policy().mode);
    mcheck(emote_mesh_div_policy().mode == kEmoteMeshDivNodeAdaptive,
           "default policy is the S5 hybrid rule");
    mcheck(divBad == 0, "div == hybrid rule (authored, else pre-S4 adaptive)");
    mcheck(fallbackNotAdaptive == 0, "missing-key parts use the adaptive fallback");
    mcheck(authoredNotNode == 0, "authored parts use exactly the node value");
    mcheck(fallbackTooCoarse == 0, "no fallback part is coarser than both rules");
    if (fallback > 0)
        mcheck(fbDivHist.size() > 0 && !(fbDivHist.size() == 1 && fbDivHist.count(8)),
               "the fallback is not the flat krkr 8 (hybrid effective)");

    // ---- arm 2: strict krkr rule (OA_EMOTE_MESHDIV=node) -----------------
    // Both halves of the hybrid stay asserted: with the strict policy the
    // missing-key nodes must take exactly krkr's 8, and the authored nodes must
    // keep their value. This is also the arm the S4 research note measured.
    {
        std::vector<EmoteDrawPart> strict;
        emote_set_mesh_div_policy(kEmoteMeshDivNode);
        const bool ok = collect(poses[0].second, &strict);
        size_t strictBad = 0, strictFallback = 0, strictImproved = 0;
        if (!ok || strict.size() != neutral.size()) {
            mcheck(false, "strict-node arm collected the same part set");
        } else {
            for (size_t k = 0; k < strict.size(); ++k) {
                const EmoteDrawPart& s = strict[k];
                if (s.meshDivision < 2) continue;
                if (s.meshDivision != node_authorised_div(s.authoredMeshDivision))
                    ++strictBad;
                if (s.authoredMeshDivision < 2) {
                    ++strictFallback;
                    if (s.meshDivision == 8) ++strictImproved;
                }
            }
        }
        std::printf("[mesh] strict node arm: fallback8=%zu/%zu strictRuleBad=%zu\n",
                    strictImproved, strictFallback, strictBad);
        mcheck(strictBad == 0, "strict arm: div == krkr rule (authored, <2 -> 8)");
        mcheck(strictImproved == strictFallback,
               "strict arm: every missing-key part takes the krkr 8");
        emote_set_mesh_div_policy(kEmoteMeshDivNodeAdaptive);
    }

    // ---- variable independence -------------------------------------------
    auto signature = [](const EmoteDrawPart& p) {
        char b[64];
        std::snprintf(b, sizeof(b), "%d/%d/%d/%d", p.source, p.icon,
                      p.authoredMeshDivision, p.meshDivision);
        return std::string(b);
    };
    // A parameter axis may add/remove a part (the frame selection changes what
    // is drawn); what must NOT change is the subdivision attached to one
    // (source,icon) draw — that is what "div depends on the node, never on the
    // variables" means. Sequence equality is reported, key equality asserted.
    size_t divChanges = 0, poseCountDiffers = 0;
    std::map<std::string, std::pair<int, int>> keyDiv; // key -> (authored, eff)
    for (size_t i = 0; i < poses.size(); ++i) {
        if (byPose[i].size() != neutral.size()) {
            ++poseCountDiffers;
            std::printf("  note: pose '%s' part count %zu != neutral %zu (frame selection)\n",
                        poses[i].first.c_str(), byPose[i].size(), neutral.size());
        }
        for (const auto& p : byPose[i]) {
            const std::string k = signature(p);
            const std::string key = k.substr(0, k.rfind('/')); // src/icon
            auto it = keyDiv.find(key);
            const std::pair<int, int> dv{p.authoredMeshDivision, p.meshDivision};
            if (it == keyDiv.end()) keyDiv.emplace(key, dv);
            else if (it->second != dv) ++divChanges;
        }
    }
    mcheck(divChanges == 0, "div independent of the variable snapshot");
    std::printf("[mesh] var-independence: poses=%zu distinctDraws=%zu divChanges=%zu "
                "partCountVaries=%zu\n", poses.size(), keyDiv.size(), divChanges,
                poseCountDiffers);

    // ---- trim reverse assertion ------------------------------------------
    // (1) the one atlas mapping is exactly (left+u*width)/texW with no extra
    //     offset/scale term — checked at three u/v probes over EVERY icon;
    // (2) every icon rect lies inside its atlas (no crop/padding packing).
    size_t iconsChecked = 0, formulaBad = 0, rectBad = 0, uvRangeBad = 0;
    for (const auto& srcp : f.sources) {
        const EmoteSource& src = *srcp;
        if (src.textureWidth <= 0 || src.textureHeight <= 0) continue;
        for (const auto& ic : src.icons) {
            ++iconsChecked;
            if (ic.left < -1e-9 || ic.top < -1e-9 ||
                ic.left + ic.width > src.textureWidth + 1e-9 ||
                ic.top + ic.height > src.textureHeight + 1e-9)
                ++rectBad;
            for (double u : {0.0, 0.25, 1.0}) {
                for (double v : {0.0, 0.5, 1.0}) {
                    double tu = 0, tv = 0;
                    oa::emote::emote_icon_uv_to_atlas(ic, src.textureWidth,
                                                             src.textureHeight, u, v, &tu,
                                                             &tv);
                    // the literal formula, written out here on purpose: a
                    // future trim compensation inside the engine helper is
                    // exactly what this must catch.
                    const double wantU = (ic.left + u * ic.width) / double(src.textureWidth);
                    const double wantV = (ic.top + v * ic.height) / double(src.textureHeight);
                    if (tu != wantU || tv != wantV) ++formulaBad;
                }
            }
        }
    }
    for (const auto& p : neutral)
        for (const auto& v : p.verts)
            if (v.u < -1e-9 || v.u > 1 + 1e-9 || v.v < -1e-9 || v.v > 1 + 1e-9)
                ++uvRangeBad;
    std::printf("[mesh] trim: icons=%zu formulaExact=%d rectInsideAtlas=%d uvIn01=%d\n",
                iconsChecked, formulaBad == 0, rectBad == 0, uvRangeBad == 0);
    mcheck(iconsChecked > 0, "icons present for the mapping assertion");
    mcheck(formulaBad == 0, "atlas uv == (left+u*width)/texW with no trim term");
    mcheck(rectBad == 0, "icon rect fully inside the atlas (no crop packing)");
    mcheck(uvRangeBad == 0, "part uv inside [0,1]");

    // ---- fidelity A/B: hybrid (default) vs strict node vs pre-S4 adaptive,
    //      all against a div=64 reference mesh -----------------------------
    // The reference arm is a converged mesh of the same evaluation: for each
    // part the deviation is sampled at the reference cell centres plus every
    // arm's own cell centres (the coarse mesh's worst points).
    const int kRefDiv = 64, kOracleDiv = 64;
    std::vector<double> errH(neutral.size(), 0.0), errNode(neutral.size(), 0.0),
        errAd(neutral.size(), 0.0);
    double worstH = 0, worstNode = 0, worstAd = 0, sumH = 0, sumNode = 0, sumAd = 0;
    size_t improved = 0, degraded = 0, equal = 0, measured = 0; // node vs adaptive
    size_t hImproved = 0, hDegraded = 0, hEqual = 0;            // hybrid vs adaptive
    size_t hVsNodeWorse = 0, hVsNodeBetter = 0, hVsNodeEqual = 0;
    size_t hFallbackNotEqualAdaptive = 0, hAuthoredNotEqualNode = 0;
    for (size_t i = 0; i < poses.size(); ++i) {
        std::vector<EmoteDrawPart> hybArm = byPose[i]; // collected under hybrid
        std::vector<EmoteDrawPart> adArm;
        emote_set_mesh_div_policy(kEmoteMeshDivAdaptive);
        if (!collect(poses[i].second, &adArm)) { ++g_meshFail; return; }
        std::vector<EmoteDrawPart> nodeArm;
        emote_set_mesh_div_policy(kEmoteMeshDivNode);
        if (!collect(poses[i].second, &nodeArm)) { ++g_meshFail; return; }
        std::vector<EmoteDrawPart> refArm;
        emote_set_mesh_div_policy(kEmoteMeshDivFixed, kRefDiv);
        if (!collect(poses[i].second, &refArm)) { ++g_meshFail; return; }
        emote_set_mesh_div_policy(kEmoteMeshDivNodeAdaptive);
        if (adArm.size() != hybArm.size() || refArm.size() != hybArm.size() ||
            nodeArm.size() != hybArm.size()) {
            mcheck(false, "A/B arms have identical part counts");
            continue;
        }
        for (size_t k = 0; k < hybArm.size(); ++k) {
            if (hybArm[k].meshDivision < 2) continue; // affine: arm-independent
            const EmoteDrawPart& rf = refArm[k];
            std::vector<std::pair<double, double>> samples;
            const int dRef = rf.meshDivision;
            for (int gy = 0; gy < dRef; ++gy)
                for (int gx = 0; gx < dRef; ++gx)
                    samples.emplace_back((gx + 0.5) / dRef, (gy + 0.5) / dRef);
            for (const EmoteDrawPart* arm : {&hybArm[k], &nodeArm[k], &adArm[k]}) {
                const int d = arm->meshDivision;
                for (int gy = 0; gy < d; ++gy)
                    for (int gx = 0; gx < d; ++gx)
                        samples.emplace_back((gx + 0.5) / d, (gy + 0.5) / d);
            }
            double eH = 0, eN = 0, eA = 0;
            for (const auto& [u, v] : samples) {
                double rx = 0, ry = 0, nx = 0, ny = 0, ax = 0, ay = 0, hx = 0, hy = 0;
                if (!mesh_sample(rf, u, v, &rx, &ry)) break;
                if (!mesh_sample(hybArm[k], u, v, &hx, &hy)) break;
                if (!mesh_sample(nodeArm[k], u, v, &nx, &ny)) break;
                if (!mesh_sample(adArm[k], u, v, &ax, &ay)) break;
                eH = std::max(eH, std::hypot(hx - rx, hy - ry));
                eN = std::max(eN, std::hypot(nx - rx, ny - ry));
                eA = std::max(eA, std::hypot(ax - rx, ay - ry));
            }
            ++measured;
            worstH = std::max(worstH, eH);
            worstNode = std::max(worstNode, eN);
            worstAd = std::max(worstAd, eA);
            sumH += eH;
            sumNode += eN;
            sumAd += eA;
            if (i == 0) { errH[k] = eH; errNode[k] = eN; errAd[k] = eA; }
            if (eN < eA - 1e-9) ++improved;
            else if (eN > eA + 1e-9) ++degraded;
            else ++equal;
            if (eH < eA - 1e-9) ++hImproved;
            else if (eH > eA + 1e-9) ++hDegraded;
            else ++hEqual;
            if (eH < eN - 1e-9) ++hVsNodeBetter;
            else if (eH > eN + 1e-9) ++hVsNodeWorse;
            else ++hVsNodeEqual;
            // The hybrid is BY CONSTRUCTION the adaptive grid on missing-key
            // nodes and the node value on authored ones — assert exactly that
            // per part (identical div => identical error).
            if (hybArm[k].authoredMeshDivision < 2) {
                if (hybArm[k].meshDivision != adArm[k].meshDivision ||
                    std::fabs(eH - eA) > 1e-9)
                    ++hFallbackNotEqualAdaptive;
            } else if (hybArm[k].meshDivision != nodeArm[k].meshDivision ||
                       std::fabs(eH - eN) > 1e-9) {
                ++hAuthoredNotEqualNode;
            }
            // A part may only be worse than the adaptive arm when the adaptive
            // rule happened to pick a FINER grid than the data authorises
            // (adaptive max 18, data has 16): documented, intentional.
            if (eH > eA + 1e-6 && hybArm[k].meshDivision >= adArm[k].meshDivision)
                mcheck(false, "hybrid arm not coarser than adaptive yet less accurate");
            if (eN > eA + 1e-6 && nodeArm[k].meshDivision >= adArm[k].meshDivision)
                mcheck(false, "node arm not coarser than adaptive yet less accurate");
        }
    }
    std::printf("[mesh] fidelity: measured(pose-parts)=%zu improved=%zu degraded=%zu "
                "equal=%zu\n", measured, improved, degraded, equal);
    std::printf("[mesh] fidelity: errHybrid max=%.4f sum=%.3f | errNode max=%.4f "
                "sum=%.3f | errAdaptive max=%.4f sum=%.3f (px, vs div=%d reference)\n",
                worstH, sumH, worstNode, sumNode, worstAd, sumAd, kRefDiv);
    std::printf("[mesh] fidelity: hybrid-vs-adaptive improved=%zu degraded=%zu equal=%zu | "
                "hybrid-vs-node better=%zu worse=%zu equal=%zu\n", hImproved, hDegraded,
                hEqual, hVsNodeBetter, hVsNodeWorse, hVsNodeEqual);
    // the coarse-grid worst points: per-part neutral table (largest 12)
    std::vector<size_t> order(neutral.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return errH[a] > errH[b];
    });
    std::printf("[mesh] neutral worst parts:\n");
    std::vector<int> adDiv(neutral.size(), 0), nodeDiv(neutral.size(), 0);
    {
        std::vector<EmoteDrawPart> adNeutral, nodeNeutral;
        emote_set_mesh_div_policy(kEmoteMeshDivAdaptive);
        collect(poses[0].second, &adNeutral);
        emote_set_mesh_div_policy(kEmoteMeshDivNode);
        collect(poses[0].second, &nodeNeutral);
        emote_set_mesh_div_policy(kEmoteMeshDivNodeAdaptive);
        if (adNeutral.size() == neutral.size())
            for (size_t k = 0; k < neutral.size(); ++k) adDiv[k] = adNeutral[k].meshDivision;
        if (nodeNeutral.size() == neutral.size())
            for (size_t k = 0; k < neutral.size(); ++k) nodeDiv[k] = nodeNeutral[k].meshDivision;
    }
    const size_t worstIdx = order.empty() ? 0 : order[0];
    for (size_t n = 0; n < order.size() && n < 12; ++n) {
        const size_t k = order[n];
        const auto& s = *f.sources[size_t(neutral[k].source)];
        std::printf("   %-24s %-26s aDiv=%3d div=%3d nodeDiv=%3d adDiv=%3d verts=%5zu "
                    "errH=%8.4f errNode=%8.4f errAdapt=%8.4f\n",
                    s.name.c_str(), s.icons[size_t(neutral[k].icon)].name.c_str(),
                    neutral[k].authoredMeshDivision, neutral[k].meshDivision, nodeDiv[k],
                    adDiv[k], neutral[k].verts.size(), errH[k], errNode[k], errAd[k]);
    }
    // S5 acceptance (research/124 §1): the hybrid must never be coarser than
    // the pre-S4 adaptive grid on a missing-key part, must keep the authored
    // parts exactly where S4 put them, and must not be worse than the strict
    // krkr fallback overall.
    mcheck(hFallbackNotEqualAdaptive == 0,
           "hybrid == adaptive on missing-key parts (no degradation vs pre-S4)");
    mcheck(hAuthoredNotEqualNode == 0, "hybrid == node value on authored parts (S4 kept)");
    mcheck(sumH <= sumNode + 1e-6 * std::max(1.0, sumNode),
           "hybrid no less accurate than strict-node overall (sum)");
    mcheck(worstH <= worstNode + 1e-6, "hybrid worst part no worse than strict-node");
    mcheck(hDegraded == 0, "no pose-part is worse under the hybrid than pre-S4");
    if (!order.empty()) {
        // Metric sanity: the deviation from an oracle mesh must fall as the
        // grid is refined (O(h^2) for a smooth surface) — if it did not, the
        // comparison above would be meaningless.
        std::vector<EmoteDrawPart> oracle;
        emote_set_mesh_div_policy(kEmoteMeshDivFixed, kOracleDiv);
        collect(poses[0].second, &oracle);
        emote_set_mesh_div_policy(kEmoteMeshDivNode);
        std::printf("[mesh] metric sanity (worst part, neutral pose, oracle div=%d):",
                    kOracleDiv);
        double prev = 1e300;
        bool monotone = true;
        if (oracle.size() == neutral.size()) {
            for (int d : {8, 16, 32}) {
                std::vector<EmoteDrawPart> arm;
                emote_set_mesh_div_policy(kEmoteMeshDivFixed, d);
                collect(poses[0].second, &arm);
                emote_set_mesh_div_policy(kEmoteMeshDivNode);
                if (arm.size() != neutral.size()) break;
                double e = 0;
                for (int gy = 0; gy < kOracleDiv; ++gy)
                    for (int gx = 0; gx < kOracleDiv; ++gx) {
                        const double u = (gx + 0.5) / kOracleDiv;
                        const double v = (gy + 0.5) / kOracleDiv;
                        double rx = 0, ry = 0, nx = 0, ny = 0;
                        if (!mesh_sample(oracle[worstIdx], u, v, &rx, &ry)) continue;
                        if (!mesh_sample(arm[worstIdx], u, v, &nx, &ny)) continue;
                        e = std::max(e, std::hypot(nx - rx, ny - ry));
                    }
                std::printf(" div=%d err=%.4f", d, e);
                if (d > 8 && e > prev + 1e-6) monotone = false;
                prev = e;
            }
        }
        std::printf("\n");
        mcheck(monotone, "mesh deviation falls monotonically with finer grids");
    }
    // ---- type-12 stencil census (research/123 §type12) --------------------
    // The reference composites any draw under a node with type==12 through a
    // mask target built from that node's stencilCompositeMaskLayerList
    // (E:emoterunner.cpp:687-697, 940-960); this engine draws it plainly.
    // The census below sizes that deviation: how many parts sit under a
    // stencil node and how much of the drawn triangle area they carry.
    {
        size_t under = 0, total = 0;
        double areaUnder = 0, areaAll = 0;
        int maxDepth = 0;
        std::map<std::string, int> layerNames;
        for (const auto& p : neutral) {
            ++total;
            double a = 0;
            for (size_t ti = 0; ti + 2 < p.verts.size(); ti += 3) {
                const auto& A = p.verts[ti];
                const auto& B = p.verts[ti + 1];
                const auto& C = p.verts[ti + 2];
                a += std::fabs((B.x - A.x) * (C.y - A.y) - (C.x - A.x) * (B.y - A.y)) * 0.5;
            }
            areaAll += a;
            if (p.type12Depth > 0) {
                ++under;
                areaUnder += a;
                maxDepth = std::max(maxDepth, p.type12Depth);
            }
        }
        // static census: type-12 nodes and their mask layer lists
        int n12 = 0, n12WithList = 0, n12Layers = 0;
        for (const auto& m : f.motions)
            for (const auto& n : m.nodes)
                if (n.type == 12) ++n12;
        std::printf("[mesh] type12: nodes=%d partsUnderStencil=%zu/%zu (depth<=%d) "
                    "triAreaShare=%.3f%%\n", n12, under, total, maxDepth,
                    areaAll > 0 ? 100.0 * areaUnder / areaAll : 0.0);
        mcheck(n12 >= 0, "type12 census ran");
    }
    // Aggregate claim (S5/research/124 §1): with the hybrid default no neutral
    // part is worse than it was before S4, the authored parts keep the S4 arm,
    // and the strict-node arm keeps its documented fallback exception.
    size_t degradedDataAuthored = 0, degradedFallback = 0; // strict node vs adaptive
    size_t hDegradedAuthored = 0, hDegradedFallback = 0;   // hybrid vs adaptive
    for (size_t k = 0; k < neutral.size(); ++k) {
        if (errNode[k] > errAd[k] + 1e-9 && neutral[k].authoredMeshDivision < 2)
            ++degradedFallback;
        else if (errNode[k] > errAd[k] + 1e-9)
            ++degradedDataAuthored;
        if (errH[k] > errAd[k] + 1e-9 && neutral[k].authoredMeshDivision < 2)
            ++hDegradedFallback;
        else if (errH[k] > errAd[k] + 1e-9)
            ++hDegradedAuthored;
    }
    std::printf("[mesh] neutral degraded split (strict node vs adaptive): authored>=2 -> "
                "%zu, fallback(<2) -> %zu\n", degradedDataAuthored, degradedFallback);
    std::printf("[mesh] neutral degraded split (hybrid vs adaptive): authored>=2 -> %zu, "
                "fallback(<2) -> %zu\n", hDegradedAuthored, hDegradedFallback);
    mcheck(!(degradedDataAuthored > 0 && worstNode > worstAd),
           "data-authorised subdivisions are the accurate arm (strict node)");
    mcheck(sumNode <= sumAd + 1e-6 * std::max(1.0, sumAd),
           "node subdivision no less accurate than adaptive (sum over parts)");
    mcheck(hDegradedAuthored + hDegradedFallback == 0,
           "hybrid default: no neutral part is worse than pre-S4");
    mcheck(worstH <= worstAd + 1e-6,
           "hybrid default: worst part no worse than pre-S4 adaptive");
    std::printf("[mesh] RESULT %s failures=%d\n", g_meshFail ? "FAIL" : "PASS", g_meshFail);
}
} // namespace

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "tay";
    const std::string mode = argc > 2 ? argv[2] : "keys";
    std::string err;
    // Two addressing shapes: the NekoMiko shortcut (`tay`/`tka`, the original
    // usage) and an explicit in-package PSB path (e.g.
    // "image\\fg\\run_0.psb"), which is served by OA_TEST_SLNY_PFS — the
    // research/132 part-level instrument for きら☆かの's E-mote portrait
    // (the modes below are data-driven, so they work on any package).
    const bool explicit_psb = which.find(".psb") != std::string::npos;
    const char* pfs = std::getenv(explicit_psb ? "OA_TEST_SLNY_PFS"
                                               : "OA_TEST_NEKOMIKO_PFS");
    if (!pfs || !*pfs) {
        std::printf("%s unset; skipping\n",
                    explicit_psb ? "OA_TEST_SLNY_PFS" : "OA_TEST_NEKOMIKO_PFS");
        return 77;
    }
    oa::fs::PhysFileSystem fs_phys(pfs, false);    oa::fs::IFileSystem& fs = fs_phys;
    const std::string file = explicit_psb
                                 ? which
                                 : ((which == "tka")
                                        ? std::string("image\\fhd\\fg\\kae\\tka_0.psb")
                                        : std::string("image\\fhd\\fg\\aya\\tay_0.psb"));
    auto bytes = fs.read(file);
    if (!bytes) {
        std::fprintf(stderr, "read %s failed\n", file.c_str());
        return 1;
    }
    EmoteFile f;
    if (!f.load(*bytes, &err)) {
        std::fprintf(stderr, "emote load: %s\n", err.c_str());
        return 1;
    }
    std::printf("== %s %s ==\n", which.c_str(), file.c_str());
    std::printf("== spec=%s atlas=%s/%dx%d chara=%s motion=%s\n",
                f.spec == SpecKind::Krkr      ? "krkr"
                : f.spec == SpecKind::Win     ? "win"
                : f.spec == SpecKind::Common  ? "common"
                                              : "unknown",
                f.sources.empty() ? "-" : f.sources[0]->textureType.c_str(),
                f.sources.empty() ? 0 : f.sources[0]->textureWidth,
                f.sources.empty() ? 0 : f.sources[0]->textureHeight,
                f.baseChara.c_str(), f.baseMotion.c_str());
    if (mode == "tl") dump_timelines(f);
    else if (mode == "mesh") {
        dump_mesh(f, which.c_str());
        return g_meshFail ? 1 : 0;
    }
    else if (mode == "sweep") dump_sweep(f, which.c_str());
    else if (mode == "params") {
        // Parameter wiring census: which variable does each parameterised node
        // actually resolve to, and which parameter ids exist but are referenced
        // by nobody? (research/132: the idle timelines of every slny character
        // animate exactly one label — body_UD — and a variable->geometry
        // response sweep says it moves nothing.)
        std::set<std::string> referenced;
        std::set<std::string> ids;
        size_t params = 0, nodes = 0, parNodes = 0, oob = 0, emptyFrames = 0;
        for (size_t mi = 0; mi < f.motions.size(); ++mi) {
            const EmoteMotion& m = f.motions[size_t(mi)];
            params += m.parameter.size();
            for (const auto& pa : m.parameter) ids.insert(pa.id);
            for (const auto& nd : m.nodes) {
                ++nodes;
                if (!nd.isParameterized) continue;
                ++parNodes;
                const bool bad = nd.parameterIndex < 0 ||
                                 nd.parameterIndex >= int(m.parameter.size());
                const std::string pid =
                    bad ? std::string("<OOB>")
                        : m.parameter[size_t(nd.parameterIndex)].id;
                if (bad) ++oob;
                if (nd.frames.empty()) ++emptyFrames;
                if (!bad) referenced.insert(pid);
                if (bad || nd.frames.empty())
                    std::printf("[params] motion#%zu '%s' node '%s' idx=%d -> %s "
                                "frames=%zu removed=%d\n",
                                mi, m.name.c_str(), nd.label.c_str(),
                                nd.parameterIndex, pid.c_str(), nd.frames.size(),
                                (int)nd.removed);
            }
        }
        std::printf("[params] motions=%zu params=%zu nodes=%zu parameterised=%zu "
                    "oob=%zu emptyFrames=%zu distinctParamIds=%zu referenced=%zu\n",
                    f.motions.size(), params, nodes, parNodes, oob, emptyFrames,
                    ids.size(), referenced.size());
        std::printf("[params] variable domain (variableNames)=%zu\n",
                    f.variableNames.size());
        std::printf("[params] unreferenced parameter ids:");
        for (const auto& id : ids)
            if (!referenced.count(id)) std::printf(" %s", id.c_str());
        std::printf("\n[params] referenced parameter ids:");
        for (const auto& id : referenced) std::printf(" %s", id.c_str());
        std::printf("\n[params] variableNames that are not a parameter id:");
        for (const auto& id : f.variableNames)
            if (!ids.count(id)) std::printf(" %s", id.c_str());
        std::printf("\n");
    }
    else if (mode == "resp") {
        // variable -> geometry response census: set ONE variable at a time and
        // report the largest part displacement vs the neutral pose (per-part
        // canvas bbox centre). This is the "is this axis wired at all" probe.
        const oa::emote::StaticRenderOptions opt = game_view();
        auto collect = [&](const std::map<std::string, double>& vars,
                           std::vector<EmoteDrawPart>* out) {
            std::string e2;
            return emote_collect_parts(f, vars, 1920, 1620, opt, out, &e2);
        };
        std::vector<EmoteDrawPart> base;
        collect({}, &base);
        std::map<std::pair<int, int>, std::array<double, 4>> b0;
        for (const auto& p : base) b0[{p.source, p.icon}] = bboxOf(p);
        std::printf("[resp] neutral parts=%zu\n", base.size());
        for (const auto& name : f.variableNames) {
            double lo = -30, hi = 30;
            for (const auto& m : f.motions)
                for (const auto& pa : m.parameter)
                    if (pa.id == name) {
                        lo = pa.rangeBegin;
                        hi = pa.rangeEnd;
                    }
            for (double probe : {hi, lo}) {
                if (name.rfind("fade_", 0) == 0) probe = (probe == hi) ? 0.0 : 1.0;
                std::vector<EmoteDrawPart> now;
                if (!collect({{name, probe}}, &now)) continue;
                double worst = 0;
                std::string who;
                for (const auto& p : now) {
                    const auto i0 = b0.find({p.source, p.icon});
                    if (i0 == b0.end()) continue;
                    const auto bb = bboxOf(p);
                    const double dx =
                        (bb[0] + bb[2]) / 2 - (i0->second[0] + i0->second[2]) / 2;
                    const double dy =
                        (bb[1] + bb[3]) / 2 - (i0->second[1] + i0->second[3]) / 2;
                    const double d = std::sqrt(dx * dx + dy * dy);
                    if (d > worst) {
                        worst = d;
                        const auto& s = *f.sources[size_t(p.source)];
                        who = s.icons[size_t(p.icon)].name;
                    }
                }
                std::printf("[resp] %-22s =%-8.2f parts=%zu worstPartMove=%.2fpx "
                            "(%s)\n",
                            name.c_str(), probe, now.size(), worst, who.c_str());
            }
        }
    }
    else if (mode == "idle") dump_idle_traj(f, which.c_str());
    else if (mode == "parts" || (mode == "one" && argc > 3)) {
        std::map<std::string, double> vars;
        if (mode == "one") {
            vars[argv[3]] = std::atof(argv[4]);
        } else {
            for (int i = 3; i + 1 < argc; i += 2)
                vars[argv[i]] = std::atof(argv[i + 1]);
        }
        std::vector<EmoteDrawPart> parts;
        const bool ok =
            emote_collect_parts(f, vars, 1920, 1620, game_view(), &parts, &err);
        std::printf("parts=%zu ok=%d\n", parts.size(), int(ok));
        if (!ok) std::printf("collect error: %s\n", err.c_str());
        double ux0 = 1e300, uy0 = 1e300, ux1 = -1e300, uy1 = -1e300;
        for (size_t i = 0; i < parts.size(); ++i) {
            const auto& pr = parts[i];
            const auto bb = bboxOf(pr);
            const auto& s = *f.sources[size_t(pr.source)];
            const std::string nm = s.icons[size_t(pr.icon)].name;
            std::printf("[part] %3zu src=%-3d %-28s %-22s div=%2d(want%2d) "
                        "verts=%5zu bbox=(%8.1f,%8.1f)-(%8.1f,%8.1f) %s\n",
                        i, pr.source, s.name.c_str(), nm.c_str(),
                        pr.meshDivision, pr.authoredMeshDivision, pr.verts.size(),
                        bb[0], bb[1], bb[2], bb[3], pr.nodePath.c_str());
            ux0 = std::min(ux0, bb[0]);
            uy0 = std::min(uy0, bb[1]);
            ux1 = std::max(ux1, bb[2]);
            uy1 = std::max(uy1, bb[3]);
        }
        if (!parts.empty())
            std::printf("union bbox=(%.1f,%.1f)-(%.1f,%.1f)\n", ux0, uy0, ux1,
                        uy1);
    } else {
        dump_keys(f);
    }
    return 0;
}
