// Report-only probe: dump the PSB's selectors and timeline variable tracks so
// an expression's eye/brow labels can be traced without the official runtime.
// usage: emote_vars_dump <tka|tay> [timeline-substring]
//        emote_vars_dump <tka|tay> render <out.ppm> [key=value ...]
// Env: OA_TEST_NEKOMIKO_PFS
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <functional>

#include "core/emote/emote_file.h"
#include "core/emote/emote_player.h"
#include "core/fs/physfs_fs.h"

static bool write_ppm(const char* path, const std::vector<uint8_t>& rgba, int w, int h) {
    FILE* fp = std::fopen(path, "wb");
    if (!fp) return false;
    std::fprintf(fp, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
            // composite over mid-grey so transparent areas are readable
            const double a = p[3] / 255.0;
            const uint8_t rgb[3] = {uint8_t(p[0] * a + 60 * (1 - a)),
                                    uint8_t(p[1] * a + 60 * (1 - a)),
                                    uint8_t(p[2] * a + 60 * (1 - a))};
            std::fwrite(rgb, 1, 3, fp);
        }
    std::fclose(fp);
    return true;
}

int main(int argc, char** argv) {
    const char* pfs = std::getenv("OA_TEST_NEKOMIKO_PFS");
    if (!pfs || !*pfs) {
        std::printf("OA_TEST_NEKOMIKO_PFS unset\n");
        return 77;
    }
    const std::string who = argc > 1 ? argv[1] : "tka";
    const std::string sub = argc > 2 ? argv[2] : "";
    const std::string entry = "image\\fhd\\fg\\" +
                              std::string(who == "tka" ? "kae\\tka_0.psb" : "aya\\tay_0.psb");
    // A direct .psb path (contains a separator or ends with .psb) is loaded from
    // disk instead of the PFS — that is how patched A/B fixtures are fed in.
    std::optional<std::vector<uint8_t>> bytes;
    if (who.find('\\') != std::string::npos || who.find('/') != std::string::npos) {
        FILE* fp = std::fopen(who.c_str(), "rb");
        if (!fp) {
            std::fprintf(stderr, "open %s failed\n", who.c_str());
            return 1;
        }
        std::fseek(fp, 0, SEEK_END);
        const long n = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        std::vector<uint8_t> buf;
        buf.resize(size_t(n));
        const size_t got = std::fread(buf.data(), 1, size_t(n), fp);
        std::fclose(fp);
        buf.resize(got);
        bytes = std::move(buf);
    } else {
        oa::fs::PhysFileSystem phys(pfs, false);
        oa::fs::IFileSystem& fs = phys;
        bytes = fs.read(entry);
        if (!bytes) {
            std::fprintf(stderr, "read %s failed\n", entry.c_str());
            return 1;
        }
    }
    oa::emote::EmoteFile f;
    std::string err;
    if (!f.load(*bytes, &err)) {
        std::fprintf(stderr, "load failed: %s\n", err.c_str());
        return 1;
    }

    if (sub == "nodeparam") {
        const std::string pid = argc > 3 ? argv[3] : "face_eye_open";
        for (const auto& m : f.motions) {
            for (size_t ni = 0; ni < m.nodes.size(); ++ni) {
                const auto& n = m.nodes[ni];
                if (!n.isParameterized || n.parameterIndex < 0 ||
                    n.parameterIndex >= int(m.parameter.size()))
                    continue;
                if (m.parameter[size_t(n.parameterIndex)].id != pid) continue;
                std::printf("[node] motion='%s' #%zu label='%s' frames=%zu\n",
                            m.name.c_str(), ni, n.label.c_str(), n.frames.size());
                for (const auto& fr : n.frames)
                    std::printf("    t=%-6g type=%d content=%d src=%s icon=%s\n", fr.time,
                                fr.type, int(fr.hasContent), fr.src.c_str(),
                                fr.icon.c_str());
            }
        }
        return 0;
    }

    if (sub == "raw") {
        // metadata.variableList entries: every authored field (our parser only
        // keeps the label and hardcodes 0 as the initial value).
        auto root = f.psb().object_member(f.psb().root_offset(), "metadata");
        if (!root) { std::printf("no metadata\n"); return 0; }
        auto vl = f.psb().object_member(*root, "variableList");
        if (!vl) { std::printf("no variableList\n"); return 0; }
        std::vector<uint32_t> items;
        f.psb().list_items(*vl, &items);
        for (uint32_t it : items) {
            std::vector<std::string> keys;
            std::vector<uint32_t> vals;
            f.psb().object_entries(it, &keys, &vals);
            std::printf("var entry:");
            for (size_t i = 0; i < keys.size(); ++i) {
                double d = 0;
                auto s = f.psb().read_string(vals[i]);
                int64_t n = 0;
                if (s) std::printf(" %s=\"%s\"", keys[i].c_str(), s->c_str());
                else if (f.psb().read_double(vals[i], &d))
                    std::printf(" %s=%g", keys[i].c_str(), d);
                else if (f.psb().read_int(vals[i], &n))
                    std::printf(" %s=%lld", keys[i].c_str(), (long long)n);
            }
            std::printf("\n");
        }
        return 0;
    }

    if (sub == "idlesweep") {
        const int ti = argc > 3 ? std::atoi(argv[3]) : 0; // 0 = 通常待機
        if (ti < 0 || ti >= int(f.timelines.size())) return 1;
#ifdef _WIN32
        _putenv_s("OA_DEBUG_EVAL", "1");
        _putenv_s("OA_EMOTE_MESHDBG", "1");
#else
        setenv("OA_DEBUG_EVAL", "1", 1);
        setenv("OA_EMOTE_MESHDBG", "1", 1);
#endif
        oa::emote::StaticRenderOptions opt;
        opt.fitToCanvas = false;
        opt.scale = 1.0;
        opt.dx = 0;
        opt.dy = 0;
        std::printf("[idle] timeline='%s'\n", f.timelines[size_t(ti)].label.c_str());
        std::printf("   t   headUD   eyeY     earY     outlineY   eye-ear  eye-outline\n");
        for (int t = 0; t <= 300; t += 15) {
            std::map<std::string, double> vars;
            for (const auto& tr : f.timelines[size_t(ti)].variables) {
                double v = 0;
                if (oa::emote::emote_sample_track(tr, t, false, &v)) vars[tr.label] = v;
            }
            std::vector<oa::emote::EmoteDrawPart> parts;
            std::string e2;
            if (!oa::emote::emote_collect_parts(f, vars, 2000, 4000, opt, &parts, &e2)) continue;
            auto ctr = [&](const char* needle, double* cy) {
                double y0 = 1e9, y1 = -1e9; int n = 0;
                for (const auto& pr : parts) {
                    if (pr.nodePath.find(needle) == std::string::npos) continue;
                    for (const auto& v : pr.verts) { y0 = std::min(y0, v.y); y1 = std::max(y1, v.y); }
                    ++n;
                }
                if (!n) return false;
                *cy = (y0 + y1) / 2;
                return true;
            };
            double ey = 0, ary = 0, oy = 0;
            const bool oe = ctr("瞳", &ey), oa = ctr("■耳L", &ary), oo = ctr("輪郭", &oy);
            auto it = vars.find("head_UD");
            std::printf("%4d %7.2f %8.1f %8.1f %9.1f %9.1f %9.1f\n", t,
                        it == vars.end() ? 0.0 : it->second, oe ? ey : 0.0, oa ? ary : 0.0,
                        oo ? oy : 0.0, (oe && oa) ? ey - ary : 0.0,
                        (oe && oo) ? ey - oy : 0.0);
        }
        return 0;
    }

    if (sub == "bp") {
        // bezier patch grids of a labelled node, per key frame
        const std::string want = argc > 3 ? argv[3] : "UD";
        for (const auto& m : f.motions)
            for (const auto& n : m.nodes) {
                if (n.label != want) continue;
                std::printf("[bp] motion='%s' label='%s' frames=%zu\n", m.name.c_str(),
                            n.label.c_str(), n.frames.size());
                for (const auto& fr : n.frames) {
                    std::printf("  t=%-5g type=%d hasBp=%d icon='%s' src='%s'\n", fr.time,
                                fr.type, int(fr.hasBp), fr.icon.c_str(), fr.src.c_str());
                    if (!fr.hasBp) continue;
                    for (int r = 0; r < 4; ++r) {
                        std::printf("    ");
                        for (int c = 0; c < 4; ++c)
                            std::printf("(%8.2f,%8.2f) ", fr.bp[(r * 4 + c) * 2],
                                        fr.bp[(r * 4 + c) * 2 + 1]);
                        std::printf("\n");
                    }
                }
            }
        return 0;
    }

    if (sub == "playpass") {
        // emulate system/adv/emote.lua playTimeline(): pass -> play -> fadeIn(idle) -> pass
        oa::emote::EmotePlayer p;
        if (!p.load(*bytes, 960, 810, &err)) {
            std::fprintf(stderr, "player load: %s\n", err.c_str());
            return 1;
        }
        const std::string tl = argc > 3 ? argv[3] : "\xe7\x85\xa7\xe3\x82\x8c_\xe3\x83\x9c\xe3\x82\xa4\xe3\x82\xb9\xe5\x86\x8d\xe7\x94\x9f\xe7\x94\xa8";
        const bool withIdle = std::getenv("OA_IDLE") != nullptr;
        p.pass();
        p.play_timeline(tl);
        if (withIdle) p.fade_in_timeline("\xe9\x80\x9a\xe5\xb8\xb8\xe5\xbe\x85\xe6\xa9\x9f");
        p.pass();
        if (std::getenv("OA_STEP_MS")) p.advance_ms(uint64_t(std::atof(std::getenv("OA_STEP_MS"))));
        for (const auto& n : f.variableNames)
            std::printf("[ours] %-20s %.3f\n", n.c_str(), p.get_variable(n.c_str()));
        return 0;
    }

    if (sub == "partsat") {
        // same path the GPU renderer uses (emote_collect_parts), so this is
        // what the running game actually draws
#ifdef _WIN32
        _putenv_s("OA_DEBUG_EVAL", "1");
#else
        setenv("OA_DEBUG_EVAL", "1", 1);
#endif
        std::map<std::string, double> vars;
        for (int i = 3; i < argc; ++i) {
            std::string kv = argv[i];
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            vars[kv.substr(0, eq)] = std::atof(kv.c_str() + eq + 1);
        }
        oa::emote::StaticRenderOptions opt;
        opt.fitToCanvas = false;
        opt.scale = 1.0;
        std::vector<oa::emote::EmoteDrawPart> parts;
        if (!oa::emote::emote_collect_parts(f, vars, 2000, 4000, opt, &parts, &err)) {
            std::fprintf(stderr, "collect failed: %s\n", err.c_str());
            return 1;
        }
        const bool allParts = std::getenv("OA_PARTS_ALL") != nullptr;
        for (const auto& pr : parts) {
            if (!allParts && pr.nodePath.find("\xe7\x9c\x89") == std::string::npos &&
                pr.nodePath.find("\xe7\x9e\xb3") == std::string::npos &&
                pr.nodePath.find("\xe9\xa0\xac") == std::string::npos)
                continue;
            const std::string iname =
                (pr.source >= 0 && pr.source < int(f.sources.size()) && pr.icon >= 0 &&
                 pr.icon < int(f.sources[size_t(pr.source)]->icons.size()))
                    ? f.sources[size_t(pr.source)]->icons[size_t(pr.icon)].name
                    : std::string("-");
            std::printf("[part] %-100s verts=%zu alpha=%.3f sg=%d mf=%d src=%d icon=%d '%s'\n",
                        pr.nodePath.c_str(), pr.verts.size(), pr.alpha, pr.stencil_group, pr.mask_group,
                        pr.source, pr.icon, iname.c_str());
            if (std::getenv("OA_PARTS_UV") && !pr.verts.empty()) {
                double u0 = 1e9, u1 = -1e9, v0 = 1e9, v1 = -1e9;
                double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
                for (const auto& v : pr.verts) {
                    u0 = std::min(u0, v.u); u1 = std::max(u1, v.u);
                    v0 = std::min(v0, v.v); v1 = std::max(v1, v.v);
                    x0 = std::min(x0, v.x); x1 = std::max(x1, v.x);
                    y0 = std::min(y0, v.y); y1 = std::max(y1, v.y);
                }
                std::printf("        uv=[%.3f..%.3f]x[%.3f..%.3f] xy=(%.1f,%.1f)-(%.1f,%.1f)\n",
                            u0, u1, v0, v1, x0, y0, x1, y1);
            }
        }
        return 0;
    }

    if (sub == "eyepos") {
        // authored anchors (charaProfile.marker) vs where the evaluated parts
        // actually land, in PSB design coordinates (no view mapping)
        std::printf("[marker] top=%.1f bottom=%.1f left=%.1f right=%.1f eye=%.1f mouth=%.1f\n",
                    f.marker.top, f.marker.bottom, f.marker.left, f.marker.right,
                    f.marker.eye, f.marker.mouth);
        std::printf("[marker] charaHeight=%.1f scale=%.3f screen=%dx%d\n", f.charaHeight,
                    f.scale, f.screenWidth, f.screenHeight);
#ifdef _WIN32
        _putenv_s("OA_DEBUG_EVAL", "1");
#else
        setenv("OA_DEBUG_EVAL", "1", 1);
#endif
        std::map<std::string, double> vars;
        std::vector<oa::emote::EmoteDrawPart> parts;
        oa::emote::StaticRenderOptions opt;
        opt.fitToCanvas = false;
        opt.scale = 1.0;
        opt.dx = 0;
        opt.dy = 0;
        if (!oa::emote::emote_collect_parts(f, vars, 2000, 4000, opt, &parts, &err)) {
            std::fprintf(stderr, "collect failed: %s\n", err.c_str());
            return 1;
        }
        auto bbox = [&](const char* needle, double* cx, double* cy, int* n) {
            double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
            *n = 0;
            for (const auto& pr : parts) {
                if (pr.nodePath.find(needle) == std::string::npos) continue;
                for (const auto& v : pr.verts) {
                    x0 = std::min(x0, v.x); x1 = std::max(x1, v.x);
                    y0 = std::min(y0, v.y); y1 = std::max(y1, v.y);
                }
                ++*n;
            }
            if (*n == 0) return false;
            *cx = (x0 + x1) / 2;
            *cy = (y0 + y1) / 2;
            printf_bbox:;
            std::printf("    bbox=(%.1f,%.1f)-(%.1f,%.1f)\n", x0, y0, x1, y1);
            return true;
        };
        double cx = 0, cy = 0; int n = 0;
        if (bbox("瞳", &cx, &cy, &n))
            std::printf("[eyepos] hitomi parts=%d center=(%.1f,%.1f) marker.eye=%.1f delta=%.1f\n",
                        n, cx, cy, f.marker.eye, cy - f.marker.eye);
        if (bbox("mabuta", &cx, &cy, &n))
            std::printf("[eyepos] mabuta parts=%d center=(%.1f,%.1f)\n", n, cx, cy);
        if (bbox("輪郭", &cx, &cy, &n))
            std::printf("[eyepos] outline parts=%d center=(%.1f,%.1f)\n", n, cx, cy);
        if (bbox("■耳L", &cx, &cy, &n))
            std::printf("[eyepos] earL parts=%d center=(%.1f,%.1f)\n", n, cx, cy);
        return 0;
    }

    if (sub == "seq") {
        // Same command language as tools/emote_official --seq (';' separated,
        // state printed after each step) so the two dumps diff line by line.
        //   idx:N[:flags] | play:LABEL | fadein:LABEL | fadeinidx:N | pass |
        //   step | skip | stop | progress:N | var:LABEL=VAL | dump
        // canvas / view for the A/B frame: OA_SEQ_W/H (default 1920x1620) and
        // OA_SEQ_VIEW="scale,x,y" (set_view_state) so a `render:` step lands on
        // the same camera the official host was given (--size/--scale/--coord).
        int seqW = 1920, seqH = 1620;
        if (const char* v = std::getenv("OA_SEQ_W")) seqW = std::atoi(v);
        if (const char* v = std::getenv("OA_SEQ_H")) seqH = std::atoi(v);
        oa::emote::EmotePlayer p;
        if (!p.load(*bytes, seqW, seqH, &err)) {
            std::fprintf(stderr, "player load: %s\n", err.c_str());
            return 1;
        }
        if (const char* v = std::getenv("OA_SEQ_VIEW")) {
            oa::emote::EmotePlayer::ViewState vs;
            std::sscanf(v, "%lf,%lf,%lf", &vs.scale, &vs.x, &vs.y);
            p.set_view_state(vs);
        }
        auto split = [](const std::string& s, char sep) {
            std::vector<std::string> out;
            size_t at = 0;
            for (;;) {
                const size_t q = s.find(sep, at);
                if (q == std::string::npos) {
                    out.push_back(s.substr(at));
                    break;
                }
                out.push_back(s.substr(at, q - at));
                at = q + 1;
            }
            return out;
        };
        auto dump = [&](const std::string& tag) {
            std::printf("[seq] == %s\n", tag.c_str());
            const auto v = p.variables();
            for (const auto& [k, val] : v)
                if (k.rfind("face_", 0) == 0 || k.rfind("head_", 0) == 0 ||
                    k.rfind("body_", 0) == 0 || k.rfind("act_", 0) == 0 ||
                    k.rfind("arm_type", 0) == 0 || k.rfind("move_", 0) == 0 ||
                    k.rfind("hair_", 0) == 0)
                    std::printf("[seq]   %-16s %.2f\n", k.c_str(), val);
            const auto slots = p.foreground_slots();
            std::printf("[seq]   playing=%zu", slots.size());
            for (const auto& s : slots)
                std::printf(" '%s'/%d", s.label.c_str(), s.flags);
            const std::string idle = p.idle_timeline();
            if (!idle.empty()) std::printf(" idle:'%s'", idle.c_str());
            std::printf("\n");
        };
        dump("create");
        const std::string script = argc > 3 ? argv[3] : std::string();
        for (std::string cmd : split(script, ';')) {
            while (!cmd.empty() && (cmd.front() == ' ' || cmd.front() == '\t'))
                cmd.erase(cmd.begin());
            while (!cmd.empty() && (cmd.back() == ' ' || cmd.back() == '\t'))
                cmd.pop_back();
            if (cmd.empty()) continue;
            const size_t c = cmd.find(':');
            const std::string op = c == std::string::npos ? cmd : cmd.substr(0, c);
            const std::string arg = c == std::string::npos ? std::string() : cmd.substr(c + 1);
            if (op == "idx") {
                const std::vector<std::string> f2 = split(arg, ':');
                const int idx = std::atoi(f2[0].c_str());
                const int flags = f2.size() > 1 ? std::atoi(f2[1].c_str()) : 0;
                if (idx < 0 || idx >= int(f.timelines.size())) {
                    std::printf("[seq] > play #%d: no such timeline\n", idx);
                    continue;
                }
                std::printf("[seq] > play #%d '%s' flags=%d\n", idx,
                            f.timelines[size_t(idx)].label.c_str(), flags);
                p.play_timeline(f.timelines[size_t(idx)].label, flags);
            } else if (op == "play" || op == "fadein" || op == "fadeout") {
                const std::vector<std::string> f2 = split(arg, ':');
                const int flags = f2.size() > 1 ? std::atoi(f2[1].c_str()) : 0;
                std::printf("[seq] > %s '%s' flags=%d\n", op.c_str(), f2[0].c_str(), flags);
                if (op == "play") p.play_timeline(f2[0], flags);
                else if (op == "fadein") p.fade_in_timeline(f2[0]);
                else p.stop_timeline(f2[0]);
            } else if (op == "fadeinidx") {
                const int idx = std::atoi(arg.c_str());
                if (idx < 0 || idx >= int(f.timelines.size())) continue;
                std::printf("[seq] > fadein #%d '%s'\n", idx,
                            f.timelines[size_t(idx)].label.c_str());
                p.fade_in_timeline(f.timelines[size_t(idx)].label);
            } else if (op == "pass") {
                std::printf("[seq] > pass\n");
                p.pass();
            } else if (op == "step") {
                std::printf("[seq] > step\n");
                p.step();
            } else if (op == "skip") {
                std::printf("[seq] > skip\n");
                p.skip();
            } else if (op == "stop") {
                std::printf("[seq] > stop\n");
                p.stop();
            } else if (op == "progress") {
                std::printf("[seq] > progress %s\n", arg.c_str());
                p.progress(std::atof(arg.c_str()));
            } else if (op == "var") {
                const size_t eq = arg.find('=');
                if (eq == std::string::npos) continue;
                std::printf("[seq] > var %s\n", arg.c_str());
                p.set_variable(arg.substr(0, eq), std::atof(arg.c_str() + eq + 1));
            } else if (op == "render") {
                // render the current pose into a PPM (same compositing as the
                // other subs) — the A/B frame the official host writes at exit
                p.render_now();
                std::printf("[seq] > render '%s' (%dx%d)\n", arg.c_str(),
                            p.width(), p.height());
                if (!write_ppm(arg.c_str(), p.rgba(), p.width(), p.height())) {
                    std::fprintf(stderr, "write %s failed\n", arg.c_str());
                    return 1;
                }
                continue;
            } else if (op == "dump") {
                std::printf("[seq] > dump\n");
            } else {
                std::printf("[seq] > UNKNOWN op '%s'\n", cmd.c_str());
                continue;
            }
            dump(cmd);
        }
        return 0;
    }

    if (sub == "nodes") {
        // dump the per-node morph flags — meshTransform decides whether a
        // blank carrier's patch deforms its children per vertex or only
        // offsets the frame (the rigid-offset class).
        std::string want = argc > 3 ? argv[3] : "";
        for (const auto& mv : f.motions) {
            for (const auto& n : mv.nodes) {
                if (!want.empty() && n.label.find(want) == std::string::npos) continue;
                std::string reg;
                for (const auto& fr : n.frames)
                    if (fr.hasBp || fr.src == "blank") { reg = fr.icon; break; }
                std::printf("[node] %-24s mot=%-14s type=%d meshTransform=%d "
                            "meshCombine=%d sync=0x%X meshDivision=%d inherit=0x%X "
                            "frames=%zu region='%s'\n",
                            n.label.c_str(), mv.name.c_str(), n.type, n.meshTransform,
                            n.meshCombine, n.meshSyncChildMask, n.meshDivision,
                            n.inheritMask, n.frames.size(), reg.c_str());
            }
        }
        return 0;
    }
    if (sub == "passsim") {
        // playTimeline -> fadeInTimeline -> pass(): the game's [fg] sequence
        oa::emote::EmotePlayer player;
        if (!player.load(*bytes, 1920, 1620, &err)) return 1;
        const std::string face = argc > 3 ? argv[3] : "9";
        const int idx = std::atoi(face.c_str());
        if (idx < 0 || idx >= int(f.timelines.size())) return 1;
        player.play_timeline(f.timelines[size_t(idx)].label, 1);
        player.fade_in_timeline(f.timelines[0].label); // 通常待機
        {
            auto v0 = player.variables();
            auto g0 = [&](const char* k) { auto it = v0.find(k); return it == v0.end() ? 0.0 : it->second; };
            std::printf("[passsim] after-play eye=%.3f brow=%.3f\n", g0("face_eye_open"), g0("face_eyebrow"));
        }
        player.pass();
        auto v = player.variables();
        auto g = [&](const char* k) { auto it = v.find(k); return it == v.end() ? 0.0 : it->second; };
        std::printf("[passsim] face=%s eye=%.3f brow=%.3f mouth=%.3f cheek=%.3f\n",
                    face.c_str(), g("face_eye_open"), g("face_eyebrow"),
                    g("face_mouth"), g("face_cheek"));
        return 0;
    }

    if (sub == "blinksim") {
        // drive the real player clock and print the eye-open variable so the
        // eyeControl blink curve can be checked against the reference lid
        oa::emote::EmotePlayer player;
        if (!player.load(*bytes, 1920, 1620, &err)) {
            std::fprintf(stderr, "player load failed: %s\n", err.c_str());
            return 1;
        }
        player.play_timeline("\xe9\x80\x9a\xe5\xb8\xb8_\xe3\x83\x9c\xe3\x82\xa4\xe3\x82\xb9\xe5\x86\x8d\xe7\x94\x9f\xe7\x94\xa8", 0);
        player.fade_in_timeline("\xe9\x80\x9a\xe5\xb8\xb8\xe5\xbe\x85\xe6\xa9\x9f");
        for (int f = 0; f < 260; ++f) {
            auto v = player.variables();
            const auto it = v.find("face_eye_open");
            std::printf("f%03d eye=%6.2f\n", f, it == v.end() ? 0.0 : it->second);
            player.progress(1.0);
        }
        return 0;
    }

    if (sub == "allkeys") {
        // every object key used anywhere in the PSB (transform vocabulary)
        const auto& psb = f.psb();
        std::map<std::string, int> hist;
        std::function<void(uint32_t, int)> walkv = [&](uint32_t off, int depth) {
            if (depth > 30) return;
            const auto k = psb.kind_at(off);
            if (k == oa::emote::Kind::Objects) {
                std::vector<std::string> keys;
                std::vector<uint32_t> vals;
                psb.object_entries(off, &keys, &vals);
                for (size_t i = 0; i < keys.size(); ++i) {
                    ++hist[keys[i]];
                    if (keys[i] != "frameList") walkv(vals[i], depth + 1);
                }
            } else if (k == oa::emote::Kind::List) {
                std::vector<uint32_t> items;
                if (psb.list_items(off, &items))
                    for (uint32_t it : items) walkv(it, depth + 1);
            }
        };
        walkv(psb.root_offset(), 0);
        for (const auto& [k, n] : hist)
            if (k.find("flip") != std::string::npos || k.find("mirror") != std::string::npos ||
                k.find("invert") != std::string::npos || k.find("scale") != std::string::npos ||
                k.find("angle") != std::string::npos || k.find("coord") != std::string::npos ||
                k.find("origin") != std::string::npos || k.find("zoom") != std::string::npos ||
                k.find("reverse") != std::string::npos || n > 50)
                std::printf("    %-24s %d\n", k.c_str(), n);
        return 0;
    }

    if (sub == "mirrorinfo") {
        // where do mirror / mirrorControl live, and what do they carry?
        const auto& psb = f.psb();
        std::function<void(uint32_t, std::string, int)> walkm =
            [&](uint32_t off, std::string path, int depth) {
                if (depth > 30) return;
                const auto k = psb.kind_at(off);
                if (k == oa::emote::Kind::Objects) {
                    std::vector<std::string> keys;
                    std::vector<uint32_t> vals;
                    psb.object_entries(off, &keys, &vals);
                    std::string label;
                    for (size_t i = 0; i < keys.size(); ++i)
                        if (keys[i] == "label")
                            if (auto sv = psb.read_string(vals[i])) label = *sv;
                    std::string here = path + (label.empty() ? "?" : label) + "/";
                    for (size_t i = 0; i < keys.size(); ++i) {
                        if (keys[i].find("mirror") == std::string::npos) continue;
                        std::printf("[mirror] %s%s = ", here.c_str(), keys[i].c_str());
                        int64_t n = 0;
                        double d = 0;
                        if (psb.read_int(vals[i], &n)) std::printf("%lld\n", (long long)n);
                        else if (psb.read_double(vals[i], &d)) std::printf("%g\n", d);
                        else std::printf("(other)\n");
                        // dump the whole object too
                        std::vector<std::string> k2;
                        std::vector<uint32_t> v2;
                        psb.object_entries(vals[i], &k2, &v2);
                        {
                            std::vector<uint32_t> lv;
                            if (psb.list_items(vals[i], &lv)) {
                                std::printf("[mirror]   list(%zu):", lv.size());
                                for (uint32_t e : lv) {
                                    int64_t nn = 0; double dd = 0;
                                    auto ss = psb.read_string(e);
                                    if (ss) std::printf(" \"%s\"", ss->c_str());
                                    else if (psb.read_int(e, &nn)) std::printf(" %lld", (long long)nn);
                                    else if (psb.read_double(e, &dd)) std::printf(" %g", dd);
                                    else std::printf(" obj");
                                }
                                std::printf("\n");
                            }
                        }
                        if (!k2.empty()) {
                            std::printf("[mirror]   object:");
                            for (size_t j = 0; j < k2.size(); ++j) {
                                int64_t nn = 0;
                                double dd = 0;
                                auto ss = psb.read_string(v2[j]);
                                if (ss) std::printf(" %s=\"%s\"", k2[j].c_str(), ss->c_str());
                                else if (psb.read_int(v2[j], &nn))
                                    std::printf(" %s=%lld", k2[j].c_str(), (long long)nn);
                                else if (psb.read_double(v2[j], &dd))
                                    std::printf(" %s=%g", k2[j].c_str(), dd);
                            }
                            std::printf("\n");
                        }
                    }
                    for (size_t i = 0; i < keys.size(); ++i)
                        if (keys[i] != "frameList") walkm(vals[i], here, depth + 1);
                } else if (k == oa::emote::Kind::List) {
                    std::vector<uint32_t> items;
                    if (psb.list_items(off, &items))
                        for (uint32_t it : items) walkm(it, path, depth + 1);
                }
            };
        walkm(psb.root_offset(), "", 0);
        return 0;
    }

    if (sub == "framekeys") {
        // raw field inventory of the frames of a labelled node (default
        // 'mabuta'): which transform keys does the PSB actually carry?
        const std::string want = argc > 3 ? argv[3] : "mabuta";
        const auto& psb = f.psb();
        std::map<std::string, int> keyhist;
        int nodes = 0, frames = 0;
        // recursive walk collecting frames of nodes whose label == want
        std::function<void(uint32_t, int)> walkval = [&](uint32_t off, int depth) {
            if (depth > 24) return;
            const auto k = psb.kind_at(off);
            if (k == oa::emote::Kind::Objects) {
                std::vector<std::string> keys;
                std::vector<uint32_t> vals;
                psb.object_entries(off, &keys, &vals);
                std::string label;
                uint32_t fl = 0;
                bool hasfl = false;
                for (size_t i = 0; i < keys.size(); ++i) {
                    if (keys[i] == "label") {
                        if (auto s = psb.read_string(vals[i])) label = *s;
                    } else if (keys[i] == "frameList") {
                        fl = vals[i];
                        hasfl = true;
                    }
                }
                if (label == want && hasfl) {
                    ++nodes;
                    std::vector<uint32_t> items;
                    if (psb.list_items(fl, &items)) {
                        for (uint32_t it : items) {
                            std::vector<std::string> fk;
                            std::vector<uint32_t> fv;
                            if (!psb.object_entries(it, &fk, &fv)) continue;
                            ++frames;
                            for (const auto& kk : fk) ++keyhist[kk];
                        }
                    }
                }
                for (size_t i = 0; i < keys.size(); ++i)
                    if (keys[i] != "frameList") walkval(vals[i], depth + 1);
            } else if (k == oa::emote::Kind::List) {
                std::vector<uint32_t> items;
                if (psb.list_items(off, &items))
                    for (uint32_t it : items) walkval(it, depth + 1);
            }
        };
        walkval(psb.root_offset(), 0);
        std::printf("[framekeys] node='%s' nodes=%d frames=%d\n", want.c_str(), nodes, frames);
        for (const auto& [k, n] : keyhist) std::printf("    %-16s %d\n", k.c_str(), n);
        return 0;
    }

    if (sub == "labels") {
        std::map<std::string, int> seen;
        for (const auto& m : f.motions)
            for (const auto& n : m.nodes) {
                if (n.label.find("mask") != std::string::npos ||
                    n.label.find("マスク") != std::string::npos ||
                    n.label.find("mabuta") != std::string::npos ||
                    n.label.find("shirome") != std::string::npos ||
                    n.label.find("輪郭") != std::string::npos)
                    std::printf("[label] '%s' type=%d children=%zu frames=%zu motion='%s'\n",
                                n.label.c_str(), n.type, n.children.size(),
                                n.frames.size(), m.name.c_str());
                (void)seen;
            }
        return 0;
    }

    if (sub == "mirror") {
        // any frame with a negative scale (a horizontally mirrored part/group)?
        int hits = 0;
        for (const auto& m : f.motions)
            for (const auto& n : m.nodes) {
                bool any = false;
                for (const auto& fr : n.frames)
                    if (fr.zx < 0 || fr.zy < 0) any = true;
                if (!any) continue;
                ++hits;
                std::printf("[mirror] motion='%s' label='%s' frames=%zu\n", m.name.c_str(),
                            n.label.c_str(), n.frames.size());
                for (const auto& fr : n.frames)
                    std::printf("    t=%-6g type=%d zx=%g zy=%g angle=%g coord=(%g,%g)\n",
                                fr.time, fr.type, fr.zx, fr.zy, fr.angle, fr.coordX, fr.coordY);
            }
        std::printf("mirror nodes: %d\n", hits);
        return 0;
    }

    if (sub == "rendert") {
        // render one expression timeline sampled at time t:
        //   rendert <out.ppm> <timeline-label-utf8> <t>
        const std::string tl = argc > 3 ? argv[3] : "";
        const double t = argc > 4 ? std::atof(argv[4]) : 0.0;
        int ti = -1;
        if (!tl.empty() && tl.find_first_not_of("0123456789") == std::string::npos)
            ti = std::atoi(tl.c_str());
        else
            ti = f.find_timeline(tl);
        if (ti < 0 || ti >= int(f.timelines.size())) {
            std::fprintf(stderr, "no timeline '%s'\n", tl.c_str());
            return 1;
        }
        std::map<std::string, double> vars;
        for (const auto& tr : f.timelines[size_t(ti)].variables) {
            double v = 0;
            if (oa::emote::emote_sample_track(tr, t, false, &v)) vars[tr.label] = v;
        }
        std::printf("[rendert] '%s' t=%g vars=%zu\n",
                    f.timelines[size_t(ti)].label.c_str(), t, vars.size());
        oa::emote::StaticRenderOptions opt;
        opt.fitToCanvas = false;
        opt.scale = std::atof(std::getenv("OA_SCALE") ? std::getenv("OA_SCALE") : "0.8");
        opt.dx = 960.0;
        const double h = who == "tka" ? 3650.0 : 3695.0;
        opt.dy = 1620.0 / 2.0 + h * opt.scale - 810.0 + 100.0;
        std::vector<uint8_t> rgba;
        if (!oa::emote::render_static_frame(f, vars, 1920, 1620, opt, &rgba, &err)) {
            std::fprintf(stderr, "render failed: %s\n", err.c_str());
            return 1;
        }
        if (argc <= 5 || !write_ppm(argv[5], rgba, 1920, 1620)) return 1;
        return 0;
    }

    if (sub == "render") {
        // same mapping the game derives: scale 0.6, layer top y from emote.lua
        std::map<std::string, double> vars;
        for (int i = 4; i < argc; ++i) {
            std::string kv = argv[i];
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            vars[kv.substr(0, eq)] = std::atof(kv.c_str() + eq + 1);
        }
        oa::emote::StaticRenderOptions opt;
        opt.fitToCanvas = false;
        opt.scale = std::atof(std::getenv("OA_SCALE") ? std::getenv("OA_SCALE") : "0.6");
        opt.dx = 960.0;
        const double h = who == "tka" ? 3650.0 : 3695.0;
        const double sc = opt.scale;
        opt.dy = 1620.0 / 2.0 + h * sc - 810.0 + 100.0;
        int W = 1920, H = 1620;
        // OA_CAM=1 reproduces the official D3D9 host camera (1280x1024 canvas,
        // SetCoord(0,1024), SetScale(0.4)) so A/B frames are pixel comparable.
        if (std::getenv("OA_CAM")) {
            W = std::atoi(std::getenv("OA_W") ? std::getenv("OA_W") : "1280");
            H = std::atoi(std::getenv("OA_H") ? std::getenv("OA_H") : "1024");
            opt.dx = std::atof(std::getenv("OA_DX") ? std::getenv("OA_DX") : "0");
            opt.dy = std::atof(std::getenv("OA_DY") ? std::getenv("OA_DY") : "1024");
        }
        std::vector<uint8_t> rgba;
        if (!oa::emote::render_static_frame(f, vars, W, H, opt, &rgba, &err)) {
            std::fprintf(stderr, "render failed: %s\n", err.c_str());
            return 1;
        }
        if (!write_ppm(argv[3], rgba, W, H)) {
            std::fprintf(stderr, "write %s failed\n", argv[3]);
            return 1;
        }
        std::printf("wrote %s\n", argv[3]);
        return 0;
    }
    std::printf("== %s timelines=%zu selectors=%zu vars=%zu\n", entry.c_str(),
                f.timelines.size(), f.selectors.size(), f.variableNames.size());

    for (const auto& sel : f.selectors) {
        std::printf("[selector] %s (%zu options)\n", sel.label.c_str(), sel.options.size());
        for (const auto& o : sel.options)
            std::printf("    opt %-24s off=%g on=%g\n", o.label.c_str(), o.offValue, o.onValue);
    }

    for (const auto& ec : f.eyeControls)
        std::printf("[eyeControl] %s begin=%g end=%g blinkFrames=%g interval=[%g..%g]\n",
                    ec.label.c_str(), ec.beginFrame, ec.endFrame, ec.blinkFrameCount,
                    ec.blinkIntervalMin, ec.blinkIntervalMax);

    for (const auto& m : f.motions) {
        if (m.parameter.empty()) continue;
        std::printf("[motion] '%s' parameters=%zu\n", m.name.c_str(), m.parameter.size());
        for (const auto& pa : m.parameter)
            std::printf("    par id=%-24s range=[%g..%g] div=%g\n", pa.id.c_str(),
                        pa.rangeBegin, pa.rangeEnd, pa.division);
        std::map<std::string, int> use;
        for (const auto& n : m.nodes)
            if (n.isParameterized && n.parameterIndex >= 0 &&
                n.parameterIndex < int(m.parameter.size()))
                ++use[m.parameter[size_t(n.parameterIndex)].id];
        for (const auto& [id, n] : use)
            std::printf("    used %-24s nodes=%d\n", id.c_str(), n);
    }

    for (const auto& tl : f.timelines) {
        if (!sub.empty() && tl.label.find(sub) == std::string::npos) continue;
        std::printf("[timeline] '%s' diff=%d last=%d loop=[%d..%d] tracks=%zu\n",
                    tl.label.c_str(), tl.diff, tl.lastTime, tl.loopBegin, tl.loopEnd,
                    tl.variables.size());
        for (const auto& tr : tl.variables) {
            std::printf("   var %-22s n=%zu", tr.label.c_str(), tr.frames.size());
            for (const auto& fr : tr.frames)
                std::printf(" [t%.0f typ=%d c=%d v=%g]", fr.time, fr.type,
                            int(fr.hasContent), fr.value);
            std::printf("\n");
        }
    }
    return 0;
}
