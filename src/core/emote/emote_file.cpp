#include "core/emote/emote_file.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

// BC7/BPTC atlas decode. Vendored (richgel999/bc7enc, MIT / public
// domain) and shipped beside its only consumer — provenance in bc7decomp.h.
#include "core/emote/bc7decomp.h"

namespace oa::emote {

namespace {
double to_double(const std::optional<std::string>& s, double dflt) {
    if (!s || s->empty()) return dflt;
    try {
        size_t used = 0;
        double v = std::stod(*s, &used);
        return used == s->size() ? v : dflt;
    } catch (...) {
        return dflt;
    }
}
bool node_frame_less(const EmoteFrame& a, const EmoteFrame& b) { return a.time < b.time; }

/// Fan a [0..rows) row space across worker threads (each row writes a
/// disjoint output span, so no synchronization beyond the joins). Row
/// workers below the threshold stay inline — one-shot decodes of tiny
/// atlases gain nothing from thread startup.
template <typename Fn>
void parallel_rows(size_t rows, size_t min_rows, Fn&& fn) {
    unsigned hw = std::thread::hardware_concurrency();
    unsigned threads = std::clamp(hw ? hw : 1u, 1u, 16u);
    if (const char* v = std::getenv("OA_EMOTE_THREADS")) {
        const int n = std::atoi(v);
        if (n >= 1 && n <= 16) threads = unsigned(n);
    }
    if (threads <= 1 || rows < min_rows) {
        fn(0, rows);
        return;
    }
    const size_t band = (rows + threads - 1) / threads;
    std::vector<std::thread> th;
    th.reserve(threads);
    for (size_t b0 = 0; b0 < rows; b0 += band) {
        const size_t b1 = std::min(rows, b0 + band);
        th.emplace_back([&fn, b0, b1] { fn(b0, b1); });
    }
    for (auto& t : th) t.join();
}
} // namespace

EmoteFile::EmoteFile() = default;
EmoteFile::~EmoteFile() = default;

bool EmoteFile::load(const std::vector<uint8_t>& data, std::string* err) {
    return load(data.data(), data.size(), err);
}

bool EmoteFile::load(const uint8_t* data, size_t size, std::string* err) {
    auto fail = [&](const char* m) {
        if (err) *err = m;
        return false;
    };
    if (!psb_.load(data, size, err)) return false;
    const uint32_t root = psb_.root_offset();
    if (psb_.kind_at(root) != Kind::Objects)
        return fail("emote: root is not an object");
    auto off = psb_.object_member(root, "spec");
    if (!off) return fail("emote: missing spec");
    auto s = psb_.read_string(*off);
    if (!s) return fail("emote: bad spec");
    if (*s == "krkr") spec = SpecKind::Krkr;
    else if (*s == "win") spec = SpecKind::Win;
    else if (*s == "common") spec = SpecKind::Common;
    else return fail("emote: unknown spec");

    if (auto v = psb_.object_member(root, "version")) {
        if (auto vs = psb_.read_string(*v)) version = to_double(vs, 0.0);
    }
    {
        off = psb_.object_member(root, "screenSize");
        if (off) {
            auto get = [&](const char* k, double* out) {
                auto o = psb_.object_member(*off, k);
                if (o) psb_.read_double(*o, out);
            };
            double w = 0, h = 0, ox = 0, oy = 0;
            get("width", &w);
            get("height", &h);
            get("originX", &ox);
            get("originY", &oy);
            screenWidth = int(w);
            screenHeight = int(h);
            screenX = int(ox);
            screenY = int(oy);
        }
    }
    off = psb_.object_member(root, "metadata");
    if (off)
        if (!parse_metadata(*off, err)) return false;
    off = psb_.object_member(root, "object");
    if (off) {
        std::vector<std::string> keys;
        std::vector<uint32_t> vals;
        if (!psb_.object_entries(*off, &keys, &vals)) return fail("emote: bad objects");
        objects.reserve(keys.size());
        for (size_t i = 0; i < keys.size(); ++i) {
            EmoteObject obj;
            obj.name = keys[i];
            // object: {type, motion:{name->motion}}
            if (auto t = psb_.object_member(vals[i], "type")) {
                int64_t ti = 0;
                psb_.read_int(*t, &ti);
                obj.type = int(ti);
            }
            if (auto m = psb_.object_member(vals[i], "motion")) {
                std::vector<std::string> mkeys;
                std::vector<uint32_t> mvals;
                if (psb_.object_entries(*m, &mkeys, &mvals)) {
                    for (size_t j = 0; j < mkeys.size(); ++j) {
                        const int midx = int(motions.size());
                        motions.emplace_back();
                        if (!parse_motion(mvals[j], obj.name, mkeys[j], err)) return false;
                        obj.motions.emplace_back(mkeys[j], midx);
                    }
                }
            }
            objects.push_back(std::move(obj));
        }
    }
    off = psb_.object_member(root, "source");
    if (off) {
        std::vector<std::string> keys;
        std::vector<uint32_t> vals;
        if (!psb_.object_entries(*off, &keys, &vals)) return fail("emote: bad sources");
        for (size_t i = 0; i < keys.size(); ++i) {
            sourceOrder.push_back(keys[i]);
            if (!parse_source(vals[i], keys[i], err)) return false;
        }
    }
    // selector initial state (krkr parity: every selector starts at option 0)
    for (auto& sel : selectors) {
        if (sel.options.empty()) continue;
        for (size_t i = 0; i < sel.options.size(); ++i) {
            const double v = i == 0 ? sel.options[i].onValue : sel.options[i].offValue;
            variableDefaults[sel.options[i].label] = v;
        }
    }
    // fresh identity for this parse (see EmoteFile::uid): the renderer keys its
    // long-lived atlas textures on it, so a game that swaps the character file
    // behind one layer id can never be served the previous file's atlas.
    static std::atomic<uint64_t> next_uid{1};
    uid_ = next_uid.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool EmoteFile::parse_metadata(uint32_t off, std::string* err) {
    if (auto b = psb_.object_member(off, "base")) {
        if (auto o = psb_.object_member(*b, "chara")) {
            if (auto s = psb_.read_string(*o)) baseChara = *s;
        }
        if (auto o = psb_.object_member(*b, "motion")) {
            if (auto s = psb_.read_string(*o)) baseMotion = *s;
        }
    }
    if (auto o = psb_.object_member(off, "scale")) psb_.read_double(*o, &scale);
    if (auto o = psb_.object_member(off, "charaProfile")) {
        if (auto h = psb_.object_member(*o, "height")) psb_.read_double(*h, &charaHeight);
        if (auto pm = psb_.object_member(*o, "pixelMarker")) {
            auto get = [&](const char* k, double* out) {
                auto p = psb_.object_member(*pm, k);
                if (p) psb_.read_double(*p, out);
            };
            get("boundsTop", &marker.top);
            get("boundsBottom", &marker.bottom);
            get("boundsLeft", &marker.left);
            get("boundsRight", &marker.right);
            get("eye", &marker.eye);
            get("mouth", &marker.mouth);
            get("bust", &marker.bust);
        }
    }
    auto read_labeled_list = [&](const char* key, std::vector<uint32_t>* out) {
        auto o = psb_.object_member(off, key);
        if (!o) return false;
        return psb_.list_items(*o, out);
    };
    std::vector<uint32_t> items;
    if (read_labeled_list("timelineControl", &items))
        for (uint32_t it : items)
            if (!parse_timeline(it, err)) return false;
    if (read_labeled_list("variableList", &items)) {
        for (uint32_t it : items) {
            if (psb_.kind_at(it) != Kind::Objects) continue;
            if (auto l = psb_.object_member(it, "label")) {
                if (auto s = psb_.read_string(*l)) {
                    variableNames.push_back(*s);
                    variableDefaults[*s] = 0.0;
                }
            }
        }
    }
    if (read_labeled_list("eyeControl", &items)) {
        for (uint32_t it : items) {
            if (psb_.kind_at(it) != Kind::Objects) continue;
            EyeControl c;
            auto get = [&](const char* k, double* out) {
                auto o = psb_.object_member(it, k);
                if (o) psb_.read_double(*o, out);
            };
            if (auto l = psb_.object_member(it, "label"))
                if (auto s = psb_.read_string(*l)) c.label = *s;
            get("beginFrame", &c.beginFrame);
            get("endFrame", &c.endFrame);
            get("blinkFrameCount", &c.blinkFrameCount);
            get("blinkIntervalMin", &c.blinkIntervalMin);
            get("blinkIntervalMax", &c.blinkIntervalMax);
            if (c.endFrame == 0) c.endFrame = c.beginFrame;
            eyeControls.push_back(std::move(c));
        }
    }
    if (read_labeled_list("selectorControl", &items)) {
        for (uint32_t it : items) {
            if (psb_.kind_at(it) != Kind::Objects) continue;
            SelectorControl c;
            if (auto l = psb_.object_member(it, "label"))
                if (auto s = psb_.read_string(*l)) c.label = *s;
            if (auto ol = psb_.object_member(it, "optionList")) {
                std::vector<uint32_t> opts;
                if (psb_.list_items(*ol, &opts)) {
                    for (uint32_t oo : opts) {
                        if (psb_.kind_at(oo) != Kind::Objects) continue;
                        SelectorOption so;
                        if (auto l = psb_.object_member(oo, "label"))
                            if (auto s = psb_.read_string(*l)) so.label = *s;
                        if (auto v = psb_.object_member(oo, "offValue"))
                            psb_.read_double(*v, &so.offValue);
                        if (auto v = psb_.object_member(oo, "onValue"))
                            psb_.read_double(*v, &so.onValue);
                        c.options.push_back(std::move(so));
                    }
                }
            }
            selectors.push_back(std::move(c));
        }
    }
    return true;
}

bool EmoteFile::parse_motion(uint32_t off, const std::string& oname,
                             const std::string& mname, std::string* err) {
    (void)oname;
    EmoteMotion& m = motions.back();
    m.name = mname;
    if (auto v = psb_.object_member(off, "lastTime")) psb_.read_double(*v, &m.lastTime);
    if (auto v = psb_.object_member(off, "loopTime")) psb_.read_double(*v, &m.loopTime);

    auto list_of = [&](const char* key, std::vector<uint32_t>* out) {
        auto o = psb_.object_member(off, key);
        if (!o) return false;
        return psb_.list_items(*o, out);
    };
    std::vector<uint32_t> tmp;
    if (list_of("layer", &tmp)) {
        for (uint32_t it : tmp) {
            const int idx = int(m.nodes.size());
            m.nodes.emplace_back();
            m.nodeListDfs.push_back(idx);
            if (!parse_node(m, idx, it, err)) return false;
            m.layer.push_back(idx);
        }
    }
    if (list_of("parameter", &tmp)) {
        for (uint32_t it : tmp) {
            if (psb_.kind_at(it) != Kind::Objects) continue;
            EmoteVar v;
            auto o = psb_.object_member(it, "id");
            if (o)
                if (auto s = psb_.read_string(*o)) v.id = *s;
            o = psb_.object_member(it, "rangeBegin");
            if (o) psb_.read_double(*o, &v.rangeBegin);
            o = psb_.object_member(it, "rangeEnd");
            if (o) psb_.read_double(*o, &v.rangeEnd);
            o = psb_.object_member(it, "division");
            if (o)
                psb_.read_double(*o, &v.division);
            else
            {
                o = psb_.object_member(it, "discretization");
                if (o)
                    psb_.read_double(*o, &v.division);
            }
            m.parameter.push_back(std::move(v));
        }
    }
    // priority reorder (draw order; krkr parity). Win spec stores it as an
    // object {content:[...]} inside a one-element list, or a plain list.
    if (list_of("priority", &tmp) && !tmp.empty()) {
        std::vector<uint32_t> pri;
        if (psb_.kind_at(tmp[0]) == Kind::Objects) {
            if (auto c = psb_.object_member(tmp[0], "content")) psb_.list_items(*c, &pri);
        } else {
            pri = tmp;
        }
        if (!pri.empty()) {
            std::vector<int> reordered;
            for (uint32_t p : pri)
                if (p < m.nodeListDfs.size()) reordered.push_back(m.nodeListDfs[p]);
            if (!reordered.empty()) m.drawOrder = std::move(reordered);
        }
    }
    if (m.drawOrder.empty()) m.drawOrder = m.nodeListDfs;
    // motion-level parameterize (may be int idx or object {id,rangeBegin..})
    if (auto p = psb_.object_member(off, "parameterize")) {
        if (psb_.kind_at(*p) == Kind::Objects) {
            // isMotion object-form: treat like parameter entry 0
            EmoteVar v;
            if (auto o = psb_.object_member(*p, "id"))
                if (auto s = psb_.read_string(*o)) v.id = *s;
            if (auto o = psb_.object_member(*p, "rangeBegin")) psb_.read_double(*o, &v.rangeBegin);
            if (auto o = psb_.object_member(*p, "rangeEnd")) psb_.read_double(*o, &v.rangeEnd);
            if (auto o = psb_.object_member(*p, "division")) psb_.read_double(*o, &v.division);
            if (!v.id.empty()) m.parameter.push_back(std::move(v));
            m.isParameterized = true;
            m.parameterIndex = 0;
        } else {
            int64_t idx = -1;
            if (psb_.read_int(*p, &idx)) {
                m.isParameterized = true;
                m.parameterIndex = int(idx);
            }
        }
    }
    if (m.isParameterized) {
        for (auto& n : m.nodes)
            if (!n.isParameterized) {
                n.isParameterized = true;
                n.parameterIndex = m.parameterIndex;
            }
    }
    // selfSyncTime (motion files only use this; harmless for emote)
    for (const auto& n : m.nodes)
        for (const auto& f : n.frames)
            if (f.hasContent && f.time > m.selfSyncTime) m.selfSyncTime = f.time;
    return true;
}

bool EmoteFile::parse_node(EmoteMotion& m, int nodeIndex, uint32_t off, std::string* err) {
    auto geti = [&](const char* k, int* out) {
        if (auto v = psb_.object_member(off, k)) {
            int64_t t = 0;
            if (psb_.read_int(*v, &t)) *out = int(t);
        }
    };
    auto& N = m.nodes[nodeIndex];
    if (auto v = psb_.object_member(off, "label"))
        if (auto s = psb_.read_string(*v)) N.label = *s;
    geti("type", &N.type);
    geti("meshCombine", &N.meshCombine);
    geti("meshDivision", &N.meshDivision);
    geti("meshTransform", &N.meshTransform);
    if (auto v = psb_.object_member(off, "inheritMask")) {
        int64_t t = 0;
        if (psb_.read_int(*v, &t)) N.inheritMask = uint32_t(t);
    }
    N.isMaskNode = N.type == 12;
    if (auto p = psb_.object_member(off, "parameterize")) {
        const Kind k = psb_.kind_at(*p);
        if (k != Kind::Objects && k != Kind::Null) {
            int64_t idx = -1;
            if (psb_.read_int(*p, &idx)) {
                N.isParameterized = true;
                N.parameterIndex = int(idx);
            }
        }
    }
    if (auto v = psb_.object_member(off, "stencilType")) {
        int64_t t = 0;
        if (psb_.read_int(*v, &t)) N.stencil_type = int(t);
    }
    if (auto v = psb_.object_member(off, "stencilCompositeMaskLayerList")) {
        std::vector<uint32_t> labels;
        if (psb_.list_items(*v, &labels)) {
            for (uint32_t lo : labels) {
                if (auto s = psb_.read_string(lo))
                    N.stencil_mask_layers.push_back(*s);
            }
        }
    }
    auto list_of = [&](const char* key, std::vector<uint32_t>* out) {
        auto o = psb_.object_member(off, key);
        if (!o) return false;
        return psb_.list_items(*o, out);
    };
    std::vector<uint32_t> tmp;
    if (list_of("frameList", &tmp)) {
        for (uint32_t it : tmp) {
            EmoteFrame f;
            if (!parse_frame(it, &f, err)) return false;
            N.frames.push_back(std::move(f));
        }
        std::stable_sort(N.frames.begin(), N.frames.end(), node_frame_less);
    }
    if (list_of("children", &tmp)) {
        std::vector<int> kids;
        kids.reserve(tmp.size());
        for (uint32_t it : tmp) {
            const int idx = int(m.nodes.size());
            m.nodes.emplace_back();
            m.nodeListDfs.push_back(idx);
            if (!parse_node(m, idx, it, err)) return false;
            kids.push_back(idx);
        }
        // re-fetch: the recursion above may have reallocated m.nodes
        for (int k : kids) m.nodes[nodeIndex].children.push_back(k);
    }
    return true;
}

bool EmoteFile::parse_frame(uint32_t off, EmoteFrame* out, std::string* err) {
    (void)err;
    if (psb_.kind_at(off) != Kind::Objects) return true;
    if (auto v = psb_.object_member(off, "time")) psb_.read_double(*v, &out->time);
    if (auto v = psb_.object_member(off, "type")) {
        int64_t t = 0;
        psb_.read_int(*v, &t);
        out->type = int(t);
    }
    auto c = psb_.object_member(off, "content");
    if (!c || psb_.kind_at(*c) == Kind::Null) return true;
    out->hasContent = true;
    auto get = [&](const char* k, double* o) {
        auto v = psb_.object_member(*c, k);
        if (v) psb_.read_double(*v, o);
    };
    if (auto v = psb_.object_member(*c, "src"))
        if (auto s = psb_.read_string(*v)) out->src = *s;
    if (auto v = psb_.object_member(*c, "icon"))
        if (auto s = psb_.read_string(*v)) out->icon = *s;
    if (auto v = psb_.object_member(*c, "mask")) psb_.read_int(*v, &out->mask);
    if (auto v = psb_.object_member(*c, "coord")) {
        std::vector<uint32_t> l;
        if (psb_.list_items(*v, &l) && l.size() >= 3) {
            psb_.read_double(l[0], &out->coordX);
            psb_.read_double(l[1], &out->coordY);
            psb_.read_double(l[2], &out->coordZ);
            out->hasCoord = true;
        }
    }
    if (auto v = psb_.object_member(*c, "angle")) {
        psb_.read_double(*v, &out->angle);
        out->hasAngle = true;
    }
    get("sx", &out->sx);
    get("sy", &out->sy);
    get("zx", &out->zx);
    get("zy", &out->zy);
    get("ox", &out->ox);
    get("oy", &out->oy);
    if (auto v = psb_.object_member(*c, "opa")) {
        psb_.read_double(*v, &out->opa);
        if (spec != SpecKind::Krkr && out->opa > 1.0) out->opa /= 255.0; // win: 0..255 int
    }
    if (auto v = psb_.object_member(*c, "color")) psb_.read_int(*v, &out->color);
    if (auto v = psb_.object_member(*c, "bm")) {
        int64_t t = 0;
        psb_.read_int(*v, &t);
        out->bm = int(t);
    }
    if (auto v = psb_.object_member(*c, "motion")) {
        out->isSubMotion = true;
        if (auto t = psb_.object_member(*v, "timeOffset")) psb_.read_double(*t, &out->timeOffset);
        // win convention: src=object name, icon=motion name
        out->subObject = out->src;
        out->subMotion = out->icon;
    } else if (!out->src.empty()) {
        // win direct-source convention: src=source name, icon=icon name
    }
    if (auto v = psb_.object_member(*c, "mesh")) {
        if (psb_.kind_at(*v) != Kind::Null) {
            if (auto bp = psb_.object_member(*v, "bp")) {
                if (psb_.kind_at(*bp) != Kind::Null) {
                    std::vector<uint32_t> l;
                    if (psb_.list_items(*bp, &l) && l.size() == 32) {
                        bool ok = true;
                        double tmp = 0;
                        for (int i = 0; i < 32; ++i) {
                            ok = psb_.read_double(l[i], &tmp) && ok;
                            out->bp[i] = float(tmp);
                        }
                        out->hasBp = ok;
                    }
                }
            }
        }
    }
    if (!out->hasBp) {
        // identity 4x4 control grid (unit patch)
        for (int gy = 0; gy < 4; ++gy)
            for (int gx = 0; gx < 4; ++gx) {
                out->bp[(gy * 4 + gx) * 2 + 0] = float(gx) / 3.0f;
                out->bp[(gy * 4 + gx) * 2 + 1] = float(gy) / 3.0f;
            }
    }
    return true;
}

bool EmoteFile::parse_timeline(uint32_t off, std::string* err) {
    (void)err;
    EmoteTimeline tl;
    auto geti = [&](const char* k, int* o) {
        auto v = psb_.object_member(off, k);
        if (v) {
            int64_t x = 0;
            if (psb_.read_int(*v, &x)) *o = int(x);
        }
    };
    if (auto v = psb_.object_member(off, "label"))
        if (auto s = psb_.read_string(*v)) tl.label = *s;
    geti("lastTime", &tl.lastTime);
    geti("loopBegin", &tl.loopBegin);
    geti("loopEnd", &tl.loopEnd);
    geti("diff", &tl.diff);
    if (auto v = psb_.object_member(off, "variableList")) {
        std::vector<uint32_t> items;
        if (psb_.list_items(*v, &items)) {
            for (uint32_t it : items) {
                if (psb_.kind_at(it) != Kind::Objects) continue;
                TimeVar tv;
                if (auto l = psb_.object_member(it, "label"))
                    if (auto s = psb_.read_string(*l)) tv.label = *s;
                if (auto fl = psb_.object_member(it, "frameList")) {
                    std::vector<uint32_t> frs;
                    if (psb_.list_items(*fl, &frs)) {
                        for (uint32_t fo : frs) {
                            if (psb_.kind_at(fo) != Kind::Objects) continue;
                            TimeVarFrame f;
                            if (auto t = psb_.object_member(fo, "time")) psb_.read_double(*t, &f.time);
                            if (auto t = psb_.object_member(fo, "type")) {
                                int64_t ti = 0;
                                psb_.read_int(*t, &ti);
                                f.type = int(ti);
                            }
                            if (auto cn = psb_.object_member(fo, "content")) {
                                if (psb_.kind_at(*cn) != Kind::Null) {
                                    f.hasContent = true;
                                    if (auto e = psb_.object_member(*cn, "easing"))
                                        psb_.read_double(*e, &f.easing);
                                    if (auto v2 = psb_.object_member(*cn, "value"))
                                        psb_.read_double(*v2, &f.value);
                                }
                            }
                            tv.frames.push_back(std::move(f));
                        }
                        std::stable_sort(tv.frames.begin(), tv.frames.end(),
                                         [](const TimeVarFrame& a, const TimeVarFrame& b) {
                                             return a.time < b.time;
                                         });
                    }
                }
                tl.variables.push_back(std::move(tv));
            }
        }
    }
    timelines.push_back(std::move(tl));
    return true;
}

bool EmoteFile::parse_source(uint32_t off, const std::string& name, std::string* err) {
    (void)err;
    auto src = std::make_unique<EmoteSource>();
    src->name = name;
    if (auto v = psb_.object_member(off, "texture")) {
        if (auto t = psb_.object_member(*v, "type"))
            if (auto s = psb_.read_string(*t)) src->textureType = *s;
        {
            double tmp = 0;
            if (auto w = psb_.object_member(*v, "width")) {
                psb_.read_double(*w, &tmp);
                src->textureWidth = int(tmp);
            }
            if (auto h = psb_.object_member(*v, "height")) {
                psb_.read_double(*h, &tmp);
                src->textureHeight = int(tmp);
            }
        }
        int32_t idx = -1;
        bool extra = false;
        if (auto p = psb_.object_member(*v, "pixel"))
            if (psb_.read_resource(*p, &idx, &extra)) {
                src->pixelChunk = idx;
                src->pixelExtra = extra;
            }
    }
    if (auto v = psb_.object_member(off, "icon")) {
        std::vector<std::string> keys;
        std::vector<uint32_t> vals;
        if (psb_.object_entries(*v, &keys, &vals)) {
            for (size_t i = 0; i < keys.size(); ++i) {
                EmoteIcon ic;
                ic.name = keys[i];
                auto get = [&](const char* k, double* o) {
                    auto m = psb_.object_member(vals[i], k);
                    if (m) psb_.read_double(*m, o);
                };
                get("originX", &ic.originX);
                get("originY", &ic.originY);
                get("width", &ic.width);
                get("height", &ic.height);
                get("left", &ic.left);
                get("top", &ic.top);
                if (auto a = psb_.object_member(vals[i], "attr")) {
                    int64_t ai = 0;
                    psb_.read_int(*a, &ai);
                    ic.attr = int(ai);
                }
                if (auto md = psb_.object_member(vals[i], "metadata")) {
                    if (auto z = psb_.object_member(*md, "zorder")) {
                        int64_t zi = 0;
                        psb_.read_int(*z, &zi);
                        ic.zorder = int(zi);
                    }
                }
                src->icons.push_back(std::move(ic));
            }
        }
    }
    sources.push_back(std::move(src));
    return true;
}

int EmoteFile::find_object(const std::string& name) const {
    for (size_t i = 0; i < objects.size(); ++i)
        if (objects[i].name == name) return int(i);
    return -1;
}

int EmoteFile::find_motion(const std::string& object, const std::string& motion) const {
    const int o = find_object(object);
    if (o < 0) return -1;
    for (const auto& [mn, idx] : objects[o].motions)
        if (mn == motion) return idx;
    return -1;
}

int EmoteFile::find_timeline(const std::string& label) const {
    for (size_t i = 0; i < timelines.size(); ++i)
        if (timelines[i].label == label) return int(i);
    return -1;
}

const EmoteSource* EmoteFile::find_source(const std::string& name) const {
    for (const auto& s : sources)
        if (s->name == name) return s.get();
    return nullptr;
}

const EmoteIcon* EmoteFile::find_icon(const EmoteSource& src, const std::string& icon) const {
    for (const auto& ic : src.icons)
        if (ic.name == icon) return &ic;
    return nullptr;
}

bool EmoteFile::ensure_atlas(EmoteSource* src, std::string* err) const {
    if (src->rgbaDecoded) return true;
    std::span<const uint8_t> bytes;
    if (src->pixelChunk < 0 ||
        !psb_.chunk_bytes(size_t(src->pixelChunk), src->pixelExtra, &bytes))
        return false;
    if (src->textureType == "DXT5") {
        if (!decode_bc3(bytes.data(), src->textureWidth, src->textureHeight, &src->rgba))
            return false;
    } else if (src->textureType == "BC7") {
        // 甜蜜女友3 E-mote atlases are BC7/BPTC (16 bytes per
        // 4x4 block; full-width block rows — atlases here are 4096/2048
        // wide, always a multiple of 4). Block rows are independent, so
        // the decode fans out across threads (a 4096x4096 atlas decodes
        // ~8x faster on an 8-core host).
        if (src->textureWidth % 4 != 0 || src->textureHeight % 4 != 0)
            return false;
        const size_t bw = size_t(src->textureWidth) / 4;
        const size_t bh = size_t(src->textureHeight) / 4;
        const size_t need = bw * bh * 16;
        if (bytes.size() < need) return false;
        src->rgba.resize(size_t(src->textureWidth) * size_t(src->textureHeight) * 4);
        uint8_t* out = src->rgba.data();
        const uint8_t* in = bytes.data();
        std::atomic<bool> ok{true};
        parallel_rows(bh, 16, [&](size_t by0, size_t by1) {
            for (size_t by = by0; by < by1; ++by) {
                for (size_t bx = 0; bx < bw; ++bx) {
                    bc7decomp::color_rgba px[16];
                    const uint8_t* blk = in + (by * bw + bx) * 16;
                    if (!bc7decomp::unpack_bc7(blk, px)) {
                        ok.store(false, std::memory_order_relaxed);
                        return;
                    }
                    for (int ty = 0; ty < 4; ++ty) {
                        uint8_t* dst =
                            out +
                            ((by * 4 + size_t(ty)) * size_t(src->textureWidth) + bx * 4) * 4;
                        for (int tx = 0; tx < 4; ++tx) {
                            dst[tx * 4 + 0] = px[ty * 4 + tx].m_comps[0];
                            dst[tx * 4 + 1] = px[ty * 4 + tx].m_comps[1];
                            dst[tx * 4 + 2] = px[ty * 4 + tx].m_comps[2];
                            dst[tx * 4 + 3] = px[ty * 4 + tx].m_comps[3];
                        }
                    }
                }
            }
        });
        if (!ok.load(std::memory_order_relaxed)) return false;
    } else if (src->textureType == "RGBA8") {
        const size_t need = size_t(src->textureWidth) * src->textureHeight * 4;
        if (bytes.size() < need) return false;
        src->rgba.assign(bytes.begin(), bytes.begin() + need);
        if (spec != SpecKind::Common)
        {
            for (size_t i = 0; i < need; i += 4)
                std::swap(src->rgba[i + 0], src->rgba[i + 2]);
        }
    } else {
        if (err) *err = "emote: unsupported atlas type " + src->textureType;
        return false;
    }
    src->rgbaDecoded = true;
    return true;
}

} // namespace oa::emote
