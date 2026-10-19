// psb — E-mote PSB container tool.
//
// Two layers, matching the engine's own split:
//   * the RAW container (any PSB: E-mote figure, KiriKiri PSB, ...) via
//     oa::emote::PsbReader — header/tables/tree/JSON;
//   * the E-mote SEMANTIC model via oa::emote::EmoteFile — objects,
//     motions, timelines, atlas sources/icons, static pose rendering.
//
// Usage:
//   psb info    <file.psb>
//   psb tree    <file.psb> [--depth N] [--max N]
//   psb json    <file.psb> [out.json]
//   psb motions <file.psb>
//   psb sources <file.psb>
//   psb icons   <file.psb> [source-name]
//   psb extract <file.psb> <outdir>          atlas PNGs + manifest.json
//   psb render  <file.psb> <out.png> [--size WxH] [--scale S] [--var k=v ...]
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h> // _mkdir (extract)
#else
#include <sys/stat.h> // mkdir (extract)
#endif

#include "core/emote/emote_file.h"
#include "core/emote/psb_reader.h"
#include "core/media/image.h"

namespace {

namespace psb = oa::emote;
namespace emote = oa::emote;

void print_usage() {
    std::fprintf(stderr,
        "usage: psb <info|tree|json|motions|timelines|controls|sources|icons|iconpng|extract|unpack|render|patch-motion> <file.psb> [args]\n"
        "  psb info    <file.psb>\n"
        "  psb tree    <file.psb> [--depth N] [--max N]     container structure\n"
        "  psb json    <file.psb> [out.json]                raw tree as JSON\n"
        "  psb motions <file.psb>                           objects/motions/nodes\n"
        "  psb sources <file.psb>                           atlas sources (type/size)\n"
        "  psb icons   <file.psb> [source]                  icon rects/origins\n"
        "  psb timelines <file.psb> [substr]                named variable animations\n"
        "  psb controls <file.psb>                          axes/selectors/eyeControl\n"
        "  psb iconpng <file.psb> <outdir>                  one PNG per icon + icons.csv\n"
        "  psb extract <file.psb> <outdir>                  atlas PNGs + manifest\n"
        "  psb unpack  <file.psb> <outdir>                  everything above in one folder\n"
        "  psb render  <file.psb> <out.png> [--size WxH] [--scale S] [--var k=v]\n"
        "      Static (tick-0) pose render; --var sets E-mote variable values\n"
        "      (e.g. --var face_talk=1 --var face_eye_open=0).\n"
        "  psb patch-motion <in.psb> <out.psb> <node-label> <old> <new>\n"
        "      Same-size in-place patch: redirect that node's sub-motion\n"
        "      reference (frame content.icon) from <old> to <new>. Builds\n"
        "      differential fixtures for the official driver.\n"
        "  psb nodetree <file.psb> [motion-substr] [label-substr]\n"
        "      Node hierarchy with the child INDICES a meshSyncChildMask bit\n"
        "      indexes, plus meshTransform/meshCombine/meshDivision.\n"
        "  psb patch-int <in.psb> <out.psb> <label> <member> <value> [nth]\n"
        "      Rewrite a numeric node member in place (same byte width).\n");
}

bool read_file(const std::string& path, std::vector<uint8_t>* out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) {
        std::fclose(f);
        return false;
    }
    out->resize(size_t(n));
    const size_t got = n ? std::fread(out->data(), 1, size_t(n), f) : 0;
    std::fclose(f);
    out->resize(got);
    return true;
}

bool write_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t put = data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return put == data.size();
}

// ---------------------------------------------------------------------------
// raw container: JSON dump
// ---------------------------------------------------------------------------
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

struct JsonWalker {
    const psb::PsbReader& r;
    std::string out;
    int max_depth;
    size_t max_items;

    void indent(int depth, int count) {
        out += '\n';
        out.append(size_t(depth + count) * 2, ' ');
    }

    void value(uint32_t off, int depth, size_t* budget) {
        const psb::Kind k = r.kind_at(off);
        if (depth > max_depth || *budget == 0) {
            out += "\"more\"";
            return;
        }
        --*budget;
        switch (k) {
            case psb::Kind::Null: out += "null"; return;
            case psb::Kind::True: out += "true"; return;
            case psb::Kind::False: out += "false"; return;
            case psb::Kind::Objects: {
                std::vector<std::string> keys;
                std::vector<uint32_t> vals;
                if (!r.object_entries(off, &keys, &vals)) {
                    out += "null";
                    return;
                }
                out += '{';
                for (size_t i = 0; i < keys.size(); ++i) {
                    if (i) out += ',';
                    indent(depth, 1);
                    out += '"' + json_escape(keys[i]) + "\": ";
                    size_t sub = max_items;
                    value(vals[i], depth + 1, &sub);
                }
                if (!keys.empty()) indent(depth, 0);
                out += '}';
                return;
            }
            case psb::Kind::List: {
                std::vector<uint32_t> vals;
                if (!r.list_items(off, &vals)) {
                    out += "null";
                    return;
                }
                out += '[';
                for (size_t i = 0; i < vals.size(); ++i) {
                    if (i) out += ',';
                    indent(depth, 1);
                    size_t sub = max_items;
                    value(vals[i], depth + 1, &sub);
                }
                if (!vals.empty()) indent(depth, 0);
                out += ']';
                return;
            }
            default: break;
        }
        if (k >= psb::Kind::ArrayN1 && k <= psb::Kind::ArrayN8) {
            std::vector<uint32_t> vals;
            if (r.array_values(off, &vals)) {
                out += '[';
                for (size_t i = 0; i < vals.size(); ++i) {
                    if (i) out += ", ";
                    out += std::to_string(vals[i]);
                }
                out += ']';
                return;
            }
        }
        if (const auto s = r.read_string(off)) {
            out += '"' + json_escape(*s) + '"';
            return;
        }
        int32_t res = -1;
        bool extra = false;
        if (k >= psb::Kind::ResourceN1 && k <= psb::Kind::ResourceN4 &&
            r.read_resource(off, &res, &extra)) {
            out += "{\"resource\": " + std::to_string(res) +
                   (extra ? ", \"extra\": true}" : "}");
            return;
        }
        double d = 0;
        if (r.read_double(off, &d)) {
            char buf[64];
            if (d == double(int64_t(d)))
                std::snprintf(buf, sizeof(buf), "%lld", (long long)d);
            else
                std::snprintf(buf, sizeof(buf), "%.6g", d);
            out += buf;
            return;
        }
        out += "null";
    }
};

int cmd_json(const std::string& path, const std::string* out_path) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, &bytes)) {
        std::fprintf(stderr, "cannot read: %s\n", path.c_str());
        return 1;
    }
    psb::PsbReader r;
    std::string err;
    if (!r.load(bytes, &err)) {
        std::fprintf(stderr, "not a PSB container: %s\n", err.c_str());
        return 1;
    }
    JsonWalker w{r, std::string(), 32, 100000};
    size_t budget = 100000;
    w.value(r.root_offset(), 0, &budget);
    const std::string text = w.out + "\n";
    if (out_path) {
        std::vector<uint8_t> data(text.begin(), text.end());
        if (!write_file(*out_path, data)) {
            std::fprintf(stderr, "cannot write: %s\n", out_path->c_str());
            return 1;
        }
        std::fprintf(stderr, "%s -> %s (%zu bytes)\n", path.c_str(), out_path->c_str(),
                     text.size());
        return 0;
    }
    std::fwrite(text.data(), 1, text.size(), stdout);
    return 0;
}

// ---------------------------------------------------------------------------
// raw container: tree / info
// ---------------------------------------------------------------------------
struct TreeWalker {
    const psb::PsbReader& r;
    int max_depth;
    size_t max_lines;
    size_t lines = 0;

    static const char* kind_name(psb::Kind k) {
        switch (k) {
            case psb::Kind::Null: return "null";
            case psb::Kind::True: return "true";
            case psb::Kind::False: return "false";
            case psb::Kind::Objects: return "object";
            case psb::Kind::List: return "list";
            case psb::Kind::Float0:
            case psb::Kind::Float:
            case psb::Kind::Double: return "float";
            default:
                if (k >= psb::Kind::NumberN0 && k <= psb::Kind::NumberN8) return "int";
                if (k >= psb::Kind::ArrayN1 && k <= psb::Kind::ArrayN8) return "array";
                if (k >= psb::Kind::StringN1 && k <= psb::Kind::StringN4) return "string";
                if (k >= psb::Kind::ResourceN1 && k <= psb::Kind::ResourceN4) return "resource";
                if (k >= psb::Kind::ExtraChunkN1 && k <= psb::Kind::ExtraChunkN4)
                    return "extra-chunk";
                return "?";
        }
    }

    std::string scalar(uint32_t off, psb::Kind k) {
        if (k >= psb::Kind::StringN1 && k <= psb::Kind::StringN4) {
            const auto s = r.read_string(off);
            if (!s) return "";
            std::string t = *s;
            if (t.size() > 60) t = t.substr(0, 57) + "...";
            for (char& c : t)
                if (c == '\n' || c == '\r') c = ' ';
            return " \"" + t + "\"";
        }
        if (k >= psb::Kind::ResourceN1 && k <= psb::Kind::ResourceN4) {
            int32_t idx = -1;
            bool extra = false;
            if (r.read_resource(off, &idx, &extra))
                return " chunk#" + std::to_string(idx) + (extra ? " (extra)" : "");
            return "";
        }
        double d = 0;
        if (r.read_double(off, &d)) {
            char buf[48];
            if (d == double(int64_t(d)))
                std::snprintf(buf, sizeof(buf), " %lld", (long long)d);
            else
                std::snprintf(buf, sizeof(buf), " %.6g", d);
            return buf;
        }
        return "";
    }

    void walk(uint32_t off, int depth, const std::string& prefix) {
        if (lines >= max_lines || depth > max_depth) return;
        const psb::Kind k = r.kind_at(off);
        ++lines;
        if (k == psb::Kind::Objects) {
            std::vector<std::string> keys;
            std::vector<uint32_t> vals;
            if (!r.object_entries(off, &keys, &vals)) {
                std::printf("%s%s (unreadable at 0x%x)\n", prefix.c_str(), kind_name(k), off);
                return;
            }
            std::printf("%s%s{%zu}\n", prefix.c_str(), kind_name(k), keys.size());
            for (size_t i = 0; i < keys.size(); ++i) {
                const psb::Kind ck = r.kind_at(vals[i]);
                ++lines;
                if (ck == psb::Kind::Objects || ck == psb::Kind::List)
                    walk(vals[i], depth + 1, prefix + "  " + keys[i] + ": ");
                else
                    std::printf("%s%s%s\n", prefix.c_str(), keys[i].c_str(),
                                scalar(vals[i], ck).c_str());
                if (lines >= max_lines) {
                    std::printf("%s... (truncated)\n", prefix.c_str());
                    return;
                }
            }
            return;
        }
        if (k == psb::Kind::List) {
            std::vector<uint32_t> vals;
            if (!r.list_items(off, &vals)) return;
            std::printf("%s%s[%zu]\n", prefix.c_str(), kind_name(k), vals.size());
            return;
        }
        if (k >= psb::Kind::ArrayN1 && k <= psb::Kind::ArrayN8) {
            std::vector<uint32_t> vals;
            r.array_values(off, &vals);
            std::printf("%s%s[%zu]\n", prefix.c_str(), kind_name(k), vals.size());
            return;
        }
        std::printf("%s%s%s\n", prefix.c_str(), kind_name(k), scalar(off, k).c_str());
    }
};

int cmd_info(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, &bytes)) {
        std::fprintf(stderr, "cannot read: %s\n", path.c_str());
        return 1;
    }
    psb::PsbReader r;
    std::string err;
    if (!r.load(bytes, &err)) {
        std::fprintf(stderr, "not a PSB container: %s\n", err.c_str());
        return 1;
    }
    const psb::Header& h = r.header();
    std::printf("file:          %s (%zu bytes)\n", path.c_str(), bytes.size());
    std::printf("version:       %u\n", unsigned(h.version));
    std::printf("encrypt:       %u\n", unsigned(h.encrypt));
    std::printf("header_length: %u\n", h.header_length);
    std::printf("names:         %zu\n", r.names().size());
    std::printf("strings:       %zu\n", r.strings().size());
    size_t chunks = 0;
    std::span<const uint8_t> span;
    while (chunks < 65536 && r.chunk_bytes(chunks, false, &span)) ++chunks;
    size_t extra = 0;
    while (extra < 65536 && r.chunk_bytes(extra, true, &span)) ++extra;
    std::printf("chunks:        %zu regular, %zu extra\n", chunks, extra);
    if (h.checksum) std::printf("checksum:      0x%08x (verified)\n", *h.checksum);

    // E-mote semantics when the container is an E-mote figure.
    emote::EmoteFile ef;
    if (ef.load(bytes, &err)) {
        const char* spec = ef.spec == emote::SpecKind::Win ? "win"
                           : ef.spec == emote::SpecKind::Krkr ? "krkr"
                           : ef.spec == emote::SpecKind::Common ? "common" : "unknown";
        std::printf("--- emote ---\n");
        std::printf("spec:          %s (version %.3f)\n", spec, ef.version);
        std::printf("screen:        %dx%d at (%d,%d)\n", ef.screenWidth, ef.screenHeight,
                     ef.screenX, ef.screenY);
        std::printf("base:          %s / %s\n", ef.baseChara.c_str(), ef.baseMotion.c_str());
        std::printf("objects:       %zu\n", ef.objects.size());
        std::printf("motions:       %zu\n", ef.motions.size());
        std::printf("timelines:     %zu\n", ef.timelines.size());
        std::printf("variables:     %zu\n", ef.variableNames.size());
        std::printf("sources:       %zu\n", ef.sources.size());
        size_t icons = 0;
        for (const auto& s : ef.sources) icons += s->icons.size();
        std::printf("icons:         %zu\n", icons);
    }
    return 0;
}

int cmd_tree(const std::string& path, int depth, size_t max_lines) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, &bytes)) {
        std::fprintf(stderr, "cannot read: %s\n", path.c_str());
        return 1;
    }
    psb::PsbReader r;
    std::string err;
    if (!r.load(bytes, &err)) {
        std::fprintf(stderr, "not a PSB container: %s\n", err.c_str());
        return 1;
    }
    TreeWalker w{r, depth, max_lines};
    w.walk(r.root_offset(), 0, "");
    return 0;
}

int cmd_motions(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, &bytes)) {
        std::fprintf(stderr, "cannot read: %s\n", path.c_str());
        return 1;
    }
    emote::EmoteFile ef;
    std::string err;
    if (!ef.load(bytes, &err)) {
        std::fprintf(stderr, "not an E-mote PSB: %s\n", err.c_str());
        return 1;
    }
    std::printf("objects: %zu, motions: %zu\n", ef.objects.size(), ef.motions.size());
    for (size_t i = 0; i < ef.objects.size(); ++i) {
        const emote::EmoteObject& o = ef.objects[i];
        std::printf("[%zu] object '%s' type=%d motions=%zu\n", i, o.name.c_str(), o.type,
                    o.motions.size());
        for (const auto& [mname, midx] : o.motions) {
            const emote::EmoteMotion& m = ef.motions[size_t(midx)];
            size_t frames = 0;
            for (const auto& n : m.nodes) frames += n.frames.size();
            std::printf("      motion '%s' nodes=%zu frames=%zu last_time=%.0f%s\n",
                        mname.c_str(), m.nodes.size(), frames, m.lastTime,
                        m.isParameterized ? " parameterized" : "");
        }
    }
    if (!ef.timelines.empty()) {
        std::printf("timelines: %zu\n", ef.timelines.size());
        for (const auto& t : ef.timelines)
            std::printf("  '%s' vars=%zu last=%d loop=[%d,%d] diff=%d\n", t.label.c_str(),
                        t.variables.size(), t.lastTime, t.loopBegin, t.loopEnd, t.diff);
    }
    if (!ef.variableNames.empty()) {
        std::printf("variables(%zu):", ef.variableNames.size());
        for (size_t i = 0; i < ef.variableNames.size(); ++i) {
            std::printf("%s%s", i ? ", " : " ", ef.variableNames[i].c_str());
            if (i >= 39 && ef.variableNames.size() > 40) {
                std::printf(", ...(%zu more)", ef.variableNames.size() - i - 1);
                break;
            }
        }
        std::printf("\n");
    }
    return 0;
}

int cmd_sources(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, &bytes)) {
        std::fprintf(stderr, "cannot read: %s\n", path.c_str());
        return 1;
    }
    emote::EmoteFile ef;
    std::string err;
    if (!ef.load(bytes, &err)) {
        std::fprintf(stderr, "not an E-mote PSB: %s\n", err.c_str());
        return 1;
    }
    std::printf("%zu sources\n", ef.sources.size());
    for (size_t i = 0; i < ef.sources.size(); ++i) {
        const emote::EmoteSource& s = *ef.sources[i];
        std::printf("[%zu] %-28s %-6s %dx%d chunk=%d%s icons=%zu\n", i, s.name.c_str(),
                    s.textureType.c_str(), s.textureWidth, s.textureHeight, s.pixelChunk,
                    s.pixelExtra ? " (extra)" : "", s.icons.size());
    }
    return 0;
}

int cmd_icons(const std::string& path, const std::string& only) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, &bytes)) {
        std::fprintf(stderr, "cannot read: %s\n", path.c_str());
        return 1;
    }
    emote::EmoteFile ef;
    std::string err;
    if (!ef.load(bytes, &err)) {
        std::fprintf(stderr, "not an E-mote PSB: %s\n", err.c_str());
        return 1;
    }
    for (const auto& sp : ef.sources) {
        const emote::EmoteSource& s = *sp;
        if (!only.empty() && s.name != only) continue;
        std::printf("source '%s' (%s %dx%d)\n", s.name.c_str(), s.textureType.c_str(),
                    s.textureWidth, s.textureHeight);
        for (const auto& ic : s.icons) {
            std::printf("  %-28s rect=(%.0f,%.0f %.0fx%.0f) origin=(%.0f,%.0f) attr=%d z=%d\n",
                        ic.name.c_str(), ic.left, ic.top, ic.width, ic.height, ic.originX,
                        ic.originY, ic.attr, ic.zorder);
        }
    }
    return 0;
}

void mkdir_p(const std::string& dir) {
    std::string cur;
    for (size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/' || dir[i] == '\\') {
            if (!cur.empty() && cur != ".") {
#if defined(_WIN32)
                _mkdir(cur.c_str());
#else
                mkdir(cur.c_str(), 0777);
#endif
            }
            if (i < dir.size()) cur += dir[i];
        } else {
            cur += dir[i];
        }
    }
}

int cmd_extract(const std::string& path, const std::string& outdir) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, &bytes)) {
        std::fprintf(stderr, "cannot read: %s\n", path.c_str());
        return 1;
    }
    emote::EmoteFile ef;
    std::string err;
    if (!ef.load(bytes, &err)) {
        std::fprintf(stderr, "not an E-mote PSB: %s\n", err.c_str());
        return 1;
    }
    mkdir_p(outdir);

    std::string manifest = "{\n  \"file\": \"" + json_escape(path) + "\",\n  \"sources\": [\n";
    size_t written = 0;
    for (size_t i = 0; i < ef.sources.size(); ++i) {
        emote::EmoteSource& s = *ef.sources[i];
        manifest += "    {\"index\": " + std::to_string(i) + ", \"name\": \"" +
                    json_escape(s.name) + "\", \"textureType\": \"" +
                    json_escape(s.textureType) + "\", \"width\": " +
                    std::to_string(s.textureWidth) + ", \"height\": " +
                    std::to_string(s.textureHeight) + ", \"icons\": [";
        for (size_t k = 0; k < s.icons.size(); ++k) {
            const emote::EmoteIcon& ic = s.icons[k];
            if (k) manifest += ", ";
            manifest += "{\"name\": \"" + json_escape(ic.name) + "\", \"rect\": [" +
                        std::to_string(int(ic.left)) + ", " + std::to_string(int(ic.top)) +
                        ", " + std::to_string(int(ic.width)) + ", " +
                        std::to_string(int(ic.height)) + "], \"origin\": [" +
                        std::to_string(int(ic.originX)) + ", " +
                        std::to_string(int(ic.originY)) + "]}";
        }
        std::string png_name = std::to_string(i) + "_" + s.name + ".png";
        for (char& c : png_name)
            if (c == '/' || c == '\\' || c == ':') c = '_';
        manifest += "], \"file\": \"" + json_escape(png_name) + "\"}";
        manifest += (i + 1 < ef.sources.size()) ? ",\n" : "\n";

        if (!ef.ensure_atlas(&s, &err)) {
            std::fprintf(stderr, "[%zu] %s: atlas decode failed: %s\n", i, s.name.c_str(),
                         err.c_str());
            continue;
        }
        const std::vector<uint8_t> png = oa::media::encode_png(
            uint32_t(s.textureWidth), uint32_t(s.textureHeight), s.rgba);
        if (png.empty()) {
            std::fprintf(stderr, "[%zu] %s: PNG encode failed\n", i, s.name.c_str());
            continue;
        }
        const std::string out = outdir + "/" + png_name;
        if (!write_file(out, png)) {
            std::fprintf(stderr, "cannot write: %s\n", out.c_str());
            continue;
        }
        ++written;
        std::fprintf(stderr, "[%zu] %-28s %s %dx%d -> %s\n", i, s.name.c_str(),
                     s.textureType.c_str(), s.textureWidth, s.textureHeight, out.c_str());
    }
    manifest += "  ]\n}\n";
    std::vector<uint8_t> mbytes(manifest.begin(), manifest.end());
    write_file(outdir + "/manifest.json", mbytes);
    std::fprintf(stderr, "%zu/%zu atlases -> %s (manifest.json)\n", written, ef.sources.size(),
                 outdir.c_str());
    return written == 0 && !ef.sources.empty() ? 1 : 0;
}

int cmd_render(const std::string& path, const std::string& out, int w, int h, double scale,
               const std::map<std::string, double>& vars) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, &bytes)) {
        std::fprintf(stderr, "cannot read: %s\n", path.c_str());
        return 1;
    }
    emote::EmoteFile ef;
    std::string err;
    if (!ef.load(bytes, &err)) {
        std::fprintf(stderr, "not an E-mote PSB: %s\n", err.c_str());
        return 1;
    }
    std::vector<uint8_t> rgba;
    emote::StaticRenderOptions opt;
    opt.scale = scale;
    opt.fitToCanvas = true;
    if (!emote::render_static_frame(ef, vars, w, h, opt, &rgba, &err)) {
        std::fprintf(stderr, "render failed: %s\n", err.c_str());
        return 1;
    }
    const std::vector<uint8_t> png =
        oa::media::encode_png(uint32_t(w), uint32_t(h), rgba);
    if (png.empty() || !write_file(out, png)) {
        std::fprintf(stderr, "cannot write PNG: %s\n", out.c_str());
        return 1;
    }
    std::fprintf(stderr, "%s -> %s (%dx%d, %zu vars)\n", path.c_str(), out.c_str(), w, h,
                 vars.size());
    return 0;
}


// ---------------------------------------------------------------------------
// semantic dumps: controls (parameter axes / selectors / eyeControl), timelines
// (the named variable animations) and the node tree. These are the three views
// an expression investigation needs: which axis a face part follows, which
// timeline key writes it, and which node/icon it selects.
// ---------------------------------------------------------------------------
std::string dump_controls(const emote::EmoteFile& ef) {
    std::string o;
    char buf[512];
    std::snprintf(buf, sizeof(buf), "variables: %zu\n\n", ef.variableNames.size());
    o += buf;
    o += "== variableList (defaults are all 0; timelines drive them) ==\n";
    for (const auto& n : ef.variableNames) o += "  " + n + "\n";

    o += "\n== parameter axes (motion / id / range / division -> tick = (v-begin)/(end-begin)*division) ==\n";
    for (const auto& m : ef.motions) {
        if (m.parameter.empty()) continue;
        o += "  motion '" + m.name + "'\n";
        for (const auto& pa : m.parameter) {
            std::snprintf(buf, sizeof(buf), "    %-24s range=[%g..%g] div=%g\n", pa.id.c_str(),
                          pa.rangeBegin, pa.rangeEnd, pa.division);
            o += buf;
        }
    }

    o += "\n== selectorControl (option -> on/off variable values) ==\n";
    for (const auto& sel : ef.selectors) {
        o += "  " + sel.label + " (" + std::to_string(sel.options.size()) + " options)\n";
        for (const auto& op : sel.options) {
            std::snprintf(buf, sizeof(buf), "    %-24s on=%g off=%g\n", op.label.c_str(),
                          op.onValue, op.offValue);
            o += buf;
        }
    }

    o += "\n== eyeControl (automatic blink over the named variable) ==\n";
    for (const auto& ec : ef.eyeControls) {
        std::snprintf(buf, sizeof(buf),
                      "  label=%s begin=%g end=%g blinkFrames=%g interval=[%g..%g]\n",
                      ec.label.c_str(), ec.beginFrame, ec.endFrame, ec.blinkFrameCount,
                      ec.blinkIntervalMin, ec.blinkIntervalMax);
        o += buf;
    }
    if (ef.eyeControls.empty()) o += "  (none)\n";
    return o;
}

std::string dump_timelines(const emote::EmoteFile& ef, const std::string& only) {
    std::string o;
    char buf[512];
    for (size_t ti = 0; ti < ef.timelines.size(); ++ti) {
        const auto& tl = ef.timelines[ti];
        if (!only.empty() && tl.label.find(only) == std::string::npos) continue;
        std::snprintf(buf, sizeof(buf), "[%zu] '%s' diff=%d last=%d loop=[%d..%d] tracks=%zu\n",
                      ti, tl.label.c_str(), tl.diff, tl.lastTime, tl.loopBegin, tl.loopEnd,
                      tl.variables.size());
        o += buf;
        for (const auto& tr : tl.variables) {
            std::snprintf(buf, sizeof(buf), "    %-22s %2zu keys", tr.label.c_str(),
                          tr.frames.size());
            o += buf;
            for (const auto& fr : tr.frames) {
                std::snprintf(buf, sizeof(buf), "  t%g=%g%s", fr.time, fr.value,
                              fr.hasContent ? "" : "(end)");
                o += buf;
            }
            o += "\n";
        }
    }
    return o;
}

void dump_nodes(const emote::EmoteMotion& m, int idx, int depth, std::string* o) {
    if (idx < 0 || idx >= int(m.nodes.size()) || depth > 64) return;
    const auto& n = m.nodes[idx];
    char buf[512];
    std::string pad(size_t(depth) * 2, ' ');
    std::string par;
    if (n.isParameterized && n.parameterIndex >= 0 &&
        n.parameterIndex < int(m.parameter.size()))
        par = " param=" + m.parameter[size_t(n.parameterIndex)].id;
    std::snprintf(buf, sizeof(buf), "%s%s [type=%d]%s frames=%zu%s\n", pad.c_str(),
                  n.label.empty() ? "?" : n.label.c_str(), n.type, par.c_str(),
                  n.frames.size(), n.removed ? " (removed)" : "");
    *o += buf;
    for (const auto& fr : n.frames) {
        std::string content;
        if (fr.isSubMotion) content = "sub=" + fr.subObject + "/" + fr.subMotion;
        else if (!fr.src.empty()) content = "src=" + fr.src + " icon=" + fr.icon;
        std::snprintf(buf, sizeof(buf),
                      "%s  t=%-6g type=%d %s coord=(%g,%g,%g) scale=(%g,%g) angle=%g opa=%g%s\n",
                      pad.c_str(), fr.time, fr.type, content.c_str(), fr.coordX, fr.coordY,
                      fr.coordZ, fr.zx, fr.zy, fr.angle, fr.opa,
                      fr.hasBp ? " mesh=bp" : "");
        *o += buf;
    }
    for (int ch : n.children) dump_nodes(m, ch, depth + 1, o);
}

std::string dump_structure(const emote::EmoteFile& ef, int max_depth) {
    std::string o;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "base: %s / %s\nobjects=%zu motions=%zu timelines=%zu vars=%zu\n\n",
                  ef.baseChara.c_str(), ef.baseMotion.c_str(), ef.objects.size(),
                  ef.motions.size(), ef.timelines.size(), ef.variableNames.size());
    o += buf;
    for (const auto& obj : ef.objects) {
        o += "object '" + obj.name + "'\n";
        for (const auto& [mname, midx] : obj.motions) {
            if (midx < 0 || midx >= int(ef.motions.size())) continue;
            const auto& m = ef.motions[size_t(midx)];
            std::snprintf(buf, sizeof(buf), "  motion '%s' nodes=%zu params=%zu\n",
                          mname.c_str(), m.nodes.size(), m.parameter.size());
            o += buf;
            if (max_depth <= 0) continue;
            for (int root : m.layer) {
                dump_nodes(m, root, 2, &o);
            }
        }
    }
    return o;
}

int cmd_timelines(const std::string& path, const std::string& only) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, &bytes)) {
        std::fprintf(stderr, "cannot read: %s\n", path.c_str());
        return 1;
    }
    emote::EmoteFile ef;
    std::string err;
    if (!ef.load(bytes, &err)) {
        std::fprintf(stderr, "not an E-mote PSB: %s\n", err.c_str());
        return 1;
    }
    const std::string s = dump_timelines(ef, only);
    std::fwrite(s.data(), 1, s.size(), stdout);
    return 0;
}

int cmd_controls(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, &bytes)) {
        std::fprintf(stderr, "cannot read: %s\n", path.c_str());
        return 1;
    }
    emote::EmoteFile ef;
    std::string err;
    if (!ef.load(bytes, &err)) {
        std::fprintf(stderr, "not an E-mote PSB: %s\n", err.c_str());
        return 1;
    }
    const std::string s = dump_controls(ef);
    std::fwrite(s.data(), 1, s.size(), stdout);
    return 0;
}

// crop every icon out of its atlas: icons/<source>/<icon>.png + icons.csv
int cmd_iconpng(const std::string& path, const std::string& outdir) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, &bytes)) {
        std::fprintf(stderr, "cannot read: %s\n", path.c_str());
        return 1;
    }
    emote::EmoteFile ef;
    std::string err;
    if (!ef.load(bytes, &err)) {
        std::fprintf(stderr, "not an E-mote PSB: %s\n", err.c_str());
        return 1;
    }
    mkdir_p(outdir);
    std::string csv = "source,icon,left,top,width,height,originX,originY,file\n";
    size_t total = 0;
    for (size_t si = 0; si < ef.sources.size(); ++si) {
        emote::EmoteSource& s = *ef.sources[si];
        if (!ef.ensure_atlas(&s, &err) || s.rgba.empty()) continue;
        std::string sname = s.name;
        for (char& c : sname)
            if (c == '/' || c == '\\' || c == ':' || c == ' ') c = '_';
        const std::string sdir = outdir + "/" + sname;
        mkdir_p(sdir);
        for (size_t ii = 0; ii < s.icons.size(); ++ii) {
            const emote::EmoteIcon& ic = s.icons[ii];
            int x0 = int(std::lround(ic.left)), y0 = int(std::lround(ic.top));
            int w = int(std::lround(ic.width)), h = int(std::lround(ic.height));
            if (w <= 0 || h <= 0) continue;
            if (x0 < 0) { w += x0; x0 = 0; }
            if (y0 < 0) { h += y0; y0 = 0; }
            if (x0 + w > s.textureWidth) w = s.textureWidth - x0;
            if (y0 + h > s.textureHeight) h = s.textureHeight - y0;
            if (w <= 0 || h <= 0) continue;
            std::vector<uint8_t> px(size_t(w) * size_t(h) * 4, 0);
            for (int y = 0; y < h; ++y)
                std::memcpy(&px[size_t(y) * size_t(w) * 4],
                            &s.rgba[(size_t(y0 + y) * size_t(s.textureWidth) + size_t(x0)) * 4],
                            size_t(w) * 4);
            const std::vector<uint8_t> png =
                oa::media::encode_png(uint32_t(w), uint32_t(h), px);
            if (png.empty()) continue;
            std::string base = ic.name.empty() ? ("icon_" + std::to_string(ii)) : ic.name;
            for (char& c : base)
                if (c == '/' || c == '\\' || c == ':' || c == ' ') c = '_';
            const std::string rel = sname + "/" + base + ".png";
            if (!write_file(outdir + "/" + rel, png)) continue;
            char line[512];
            std::snprintf(line, sizeof(line), "%s,%s,%d,%d,%d,%d,%g,%g,%s\n", s.name.c_str(),
                          ic.name.c_str(), x0, y0, w, h, ic.originX, ic.originY, rel.c_str());
            csv += line;
            ++total;
        }
    }
    write_file(outdir + "/icons.csv", std::vector<uint8_t>(csv.begin(), csv.end()));
    std::fprintf(stderr, "%zu icons -> %s (icons.csv)\n", total, outdir.c_str());
    return total == 0 ? 1 : 0;
}

int cmd_unpack(const std::string& path, const std::string& outdir) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, &bytes)) {
        std::fprintf(stderr, "cannot read: %s\n", path.c_str());
        return 1;
    }
    emote::EmoteFile ef;
    std::string err;
    if (!ef.load(bytes, &err)) {
        std::fprintf(stderr, "not an E-mote PSB: %s\n", err.c_str());
        return 1;
    }
    mkdir_p(outdir);

    size_t icon_total = 0;
    for (const auto& s : ef.sources) icon_total += s->icons.size();
    std::string info;
    {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "file: %s (%zu bytes)\nversion: %g\nscreen: %dx%d\nbase: %s / %s\n"
                      "objects=%zu motions=%zu timelines=%zu variables=%zu sources=%zu icons=%zu\n",
                      path.c_str(), bytes.size(), ef.version, ef.screenWidth, ef.screenHeight,
                      ef.baseChara.c_str(), ef.baseMotion.c_str(), ef.objects.size(),
                      ef.motions.size(), ef.timelines.size(), ef.variableNames.size(),
                      ef.sources.size(), icon_total);
        info += buf;
    }
    auto w = [&](const std::string& name, const std::string& body) {
        write_file(outdir + "/" + name, std::vector<uint8_t>(body.begin(), body.end()));
    };
    w("info.txt", info);
    w("controls.txt", dump_controls(ef));
    w("timelines.txt", dump_timelines(ef, ""));
    w("structure.txt", dump_structure(ef, 64));

    // raw container tree as JSON (machine-readable, one line per value)
    {
        const std::string json_path = outdir + "/tree.json";
        cmd_json(path, &json_path);
    }
    cmd_extract(path, outdir + "/atlas");
    cmd_iconpng(path, outdir + "/icons");

    const std::string readme =
        "PSB unpack / E-mote figure package\r\n"
        "================================\r\n"
        "\r\n"
        "info.txt        container + model summary\r\n"
        "structure.txt   object -> motion -> node tree (label/type/param/frames/coords)\r\n"
        "timelines.txt   named variable animations (expression tracks: t=value keys)\r\n"
        "controls.txt    variableList / parameter axes / selectors / eyeControl\r\n"
        "tree.json       raw PSB container tree (every object value)\r\n"
        "atlas/          decoded texture atlases (DXT5 -> RGBA PNG) + manifest.json\r\n"
        "icons/          every icon cropped from its atlas, icons.csv has the rects\r\n"
        "\r\n"
        "How an expression resolves:/r/n"
        "  timeline track (timelines.txt)  -> variable value\r\n"
        "  variable -> parameter axis (controls.txt: id/range/division)\r\n"
        "  tick = (v - rangeBegin) / (rangeEnd - rangeBegin) * division\r\n"
        "  parameterized node (structure.txt: param=<id>) picks the frame at that tick\r\n"
        "  frame content -> icon (icons/<source>/<icon>.png)\r\n"
        "\r\n"
        "Eye lid example (tka_0.psb): face_eye_open range=[-10..50] div=60, so\r\n"
        "  v=0 -> tick 10 -> icon 0050 (open); v=5 -> tick 15 -> 0051 (half);\r\n"
        "  v=10 -> tick 20 -> 0052 (closed).\r\n";
    w("README.txt", readme);
    std::fprintf(stderr, "unpacked -> %s\n", outdir.c_str());
    return 0;
}


// ---------------------------------------------------------------------------
// container patcher (A/B fixtures)
//
// A same-size, in-place edit keeps every table/offset valid, so a patched file
// is still a legal PSB — no serializer needed. Used to build differential
// fixtures for the official E-mote driver: e.g. redirect one node's sub-motion
// reference to a simple motion, so the two runtimes can be compared on a
// subtree that is known-simple.
// ---------------------------------------------------------------------------
struct PatchCtx {
    std::string label;   // node label to target ("-" = any node)
    int from_index = -1; // match by pool index (>=0) ...
    std::string from;    // ... or by text
    int to_index = -1;   // replacement by pool index (>=0) ...
    std::string to;      // ... or by text
    int patched = 0;
    int frames_seen = 0;
};

int parse_index_token(const std::string& s) {
    if (s.size() > 1 && s[0] == '#') return std::atoi(s.c_str() + 1);
    return -1;
}

bool patch_string_value(psb::PsbReader& r, uint32_t off, int idx, std::string* err) {
    const psb::Kind k = r.kind_at(off);
    if (k < psb::Kind::StringN1 || k > psb::Kind::StringN4) { *err = "not a string value"; return false; }
    const int w = int(uint8_t(k) - uint8_t(psb::Kind::StringN1)) + 1;
    if (idx < 0 || idx >= int(r.strings().size())) { *err = "string index out of range"; return false; }
    if (w < 4 && uint64_t(idx) >= (uint64_t(1) << (8 * w))) { *err = "index does not fit"; return false; }
    auto& b = r.mutable_bytes();
    for (int i = 0; i < w; ++i) b[size_t(off) + 1 + size_t(i)] = uint8_t((uint32_t(idx) >> (8 * i)) & 0xff);
    return true;
}

void patch_walk(psb::PsbReader& r, uint32_t off, PatchCtx* ctx, int depth, std::string* err) {
    if (depth > 96 || !err->empty()) return;
    const psb::Kind k = r.kind_at(off);
    if (k == psb::Kind::Objects) {
        std::vector<std::string> keys;
        std::vector<uint32_t> vals;
        if (!r.object_entries(off, &keys, &vals)) return;
        bool is_target = false;
        uint32_t frame_list = 0;
        bool has_fl = false;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == "label") {
                if (auto s = r.read_string(vals[i]); s && (ctx->label == "-" || *s == ctx->label))
                    is_target = true;
            } else if (keys[i] == "frameList") {
                frame_list = vals[i];
                has_fl = true;
            }
        }
        if ((is_target || ctx->label == "-") && has_fl) {
            std::vector<uint32_t> frames;
            if (r.list_items(frame_list, &frames)) {
                for (uint32_t fr : frames) {
                    const auto content = r.object_member(fr, "content");
                    if (!content) continue;
                    const auto icon = r.object_member(*content, "icon");
                    if (!icon) continue;
                    const auto cur = r.read_string(*icon);
                    if (!cur) continue;
                    ++ctx->frames_seen;
                    if (ctx->from_index >= 0) {
                        const psb::Kind ik = r.kind_at(*icon);
                        const int iw = int(uint8_t(ik) - uint8_t(psb::Kind::StringN1)) + 1;
                        int64_t raw = 0;
                        for (int b = 0; b < iw; ++b)
                            raw |= int64_t(r.mutable_bytes()[size_t(*icon) + 1 + size_t(b)]) << (8 * b);
                        if (int(raw) != ctx->from_index) continue;
                    } else if (*cur != ctx->from) {
                        continue;
                    }
                    const int to_idx = ctx->to_index >= 0 ? ctx->to_index : r.string_index(ctx->to);
                    if (!patch_string_value(r, *icon, to_idx, err)) return;
                    ++ctx->patched;
                }
            }
        }
        for (size_t i = 0; i < vals.size(); ++i) patch_walk(r, vals[i], ctx, depth + 1, err);
    } else if (k == psb::Kind::List) {
        std::vector<uint32_t> items;
        if (r.list_items(off, &items))
            for (uint32_t v : items) patch_walk(r, v, ctx, depth + 1, err);
    }
}

int cmd_strings(const std::string& path) {
    std::vector<uint8_t> data;
    if (!read_file(path, &data)) return 1;
    psb::PsbReader r;
    std::string err;
    if (!r.load(data, &err)) { std::fprintf(stderr, "load: %s\n", err.c_str()); return 1; }
    const auto& pool = r.strings();
    for (size_t i = 0; i < pool.size(); ++i)
        std::printf("#%zu\t%s\n", i, pool[i].c_str());
    std::fprintf(stderr, "%zu strings\n", pool.size());
    return 0;
}

int cmd_patch_motion(const std::string& in, const std::string& out, const std::string& label,
                     const std::string& from, const std::string& to) {
    std::vector<uint8_t> data;
    if (!read_file(in, &data)) {
        std::fprintf(stderr, "cannot read %s\n", in.c_str());
        return 1;
    }
    psb::PsbReader r;
    std::string err;
    if (!r.load(data, &err)) {
        std::fprintf(stderr, "load: %s\n", err.c_str());
        return 1;
    }
    PatchCtx ctx;
    ctx.label = label;
    ctx.from_index = parse_index_token(from);
    ctx.to_index = parse_index_token(to);
    ctx.from = ctx.from_index >= 0 ? std::string() : from;
    ctx.to = ctx.to_index >= 0 ? std::string() : to;
    if (ctx.to_index < 0 && r.string_index(to) < 0) {
        std::fprintf(stderr, "target string '%s' is not in the pool\n", to.c_str());
        return 1;
    }
    std::string perr;
    patch_walk(r, r.root_offset(), &ctx, 0, &perr);
    if (!perr.empty()) {
        std::fprintf(stderr, "patch failed: %s\n", perr.c_str());
        return 1;
    }
    if (ctx.patched == 0) {
        std::fprintf(stderr, "no frame matched (label='%s', icon='%s'); frames seen under that label: %d\n",
                     label.c_str(), from.c_str(), ctx.frames_seen);
        return 1;
    }
    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
    auto& bytes = r.mutable_bytes();
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    std::printf("patched %d frame(s): node '%s' icon '%s' -> '%s'  => %s\n",
                ctx.patched, label.c_str(), from.c_str(), to.c_str(), out.c_str());
    return 0;
}


// ---------------------------------------------------------------------------
// numeric patcher: overwrite every morph grid (mesh.bp) with the identity grid.
//
// A differential fixture for "is the deformation the thing that differs?": an
// all-identity PSB has no morph at all, so both runtimes must agree exactly if
// their plain transform / compositing path is equivalent. Anything that still
// differs afterwards is NOT a warp problem.
// ---------------------------------------------------------------------------
bool write_real(psb::PsbReader& r, uint32_t off, double value, std::string* err) {
    const psb::Kind k = r.kind_at(off);
    auto& b = r.mutable_bytes();
    switch (k) {
        // Float0 is the compact encoding of the value 0.0 (one byte, no
        // payload) — patching it is only possible when the target is 0 too.
        case psb::Kind::Float0:
            if (value == 0.0) return true;
            *err = "Float0 cannot hold a non-zero value";
            return false;
        case psb::Kind::Float: {
            const float f = float(value);
            std::memcpy(&b[size_t(off) + 1], &f, 4);
            return true;
        }
        case psb::Kind::Double: {
            std::memcpy(&b[size_t(off) + 1], &value, 8);
            return true;
        }
        default: break;
    }
    if (k >= psb::Kind::NumberN0 && k <= psb::Kind::NumberN8) {
        const int w = int(uint8_t(k) - uint8_t(psb::Kind::NumberN0));
        const int64_t iv = (int64_t)std::llround(value);
        for (int i = 0; i < w; ++i)
            b[size_t(off) + 1 + size_t(i)] = uint8_t((uint64_t(iv) >> (8 * i)) & 0xff);
        return true;
    }
    *err = "bp item is not numeric";
    return false;
}

struct IdentCtx {
    int grids = 0;
    int items = 0;
    int skipped = 0;
    std::string keep;      // keep this node's grids, flatten everything else
    int keepIndex = -1;    // ... matched by pool index ("#123")
    int matched = 0;
};

int label_index(psb::PsbReader& r, uint32_t off) {
    const psb::Kind k = r.kind_at(off);
    if (k < psb::Kind::StringN1 || k > psb::Kind::StringN4) return -1;
    const int w = int(uint8_t(k) - uint8_t(psb::Kind::StringN1)) + 1;
    int64_t raw = 0;
    for (int b = 0; b < w; ++b)
        raw |= int64_t(r.mutable_bytes()[size_t(off) + 1 + size_t(b)]) << (8 * b);
    return int(raw);
}

bool write_real(psb::PsbReader& r, uint32_t off, double value, std::string* err);

// flatten every morph grid except the nodes selected by ctx.keep (#index or
// a UTF-8 substring of the label). One carrier left standing per run is how a
// chain gets attributed: whichever isolated carrier still differs between the
// runtimes is the one whose evaluation differs.
void ident_walk(psb::PsbReader& r, uint32_t off, IdentCtx* ctx, int depth, std::string* err) {
    if (depth > 96 || !err->empty()) return;
    const psb::Kind k = r.kind_at(off);
    if (k == psb::Kind::Objects) {
        std::vector<std::string> keys;
        std::vector<uint32_t> vals;
        if (!r.object_entries(off, &keys, &vals)) return;
        uint32_t frameList = 0;
        bool hasFrames = false;
        bool selected = false;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == "label") {
                const auto lab = r.read_string(vals[i]);
                const int idx = label_index(r, vals[i]);
                if (ctx->keepIndex >= 0)
                    selected = idx == ctx->keepIndex;
                else if (!ctx->keep.empty() && lab)
                    selected = lab->find(ctx->keep) != std::string::npos;
                if (selected) ++ctx->matched;
            } else if (keys[i] == "frameList") {
                frameList = vals[i];
                hasFrames = true;
            }
        }
        if (hasFrames && ctx->keep.rfind("reg:", 0) == 0) {
            // select by the blank region text of any frame
            std::vector<uint32_t> frames;
            const std::string want = ctx->keep.substr(4);
            if (r.list_items(frameList, &frames)) {
                for (uint32_t fr : frames) {
                    const auto content = r.object_member(fr, "content");
                    if (!content) continue;
                    const auto icon = r.object_member(*content, "icon");
                    if (!icon) continue;
                    const auto ic = r.read_string(*icon);
                    if (ic && ic->rfind(want, 0) == 0) { selected = true; ++ctx->matched; break; }
                }
            }
        }
        if (hasFrames && !selected) {
            std::vector<uint32_t> frames;
            if (r.list_items(frameList, &frames)) {
                for (uint32_t fr : frames) {
                    const auto content = r.object_member(fr, "content");
                    if (!content) continue;
                    const auto mesh = r.object_member(*content, "mesh");
                    if (!mesh) continue;
                    const auto bp = r.object_member(*mesh, "bp");
                    if (!bp) continue;
                    std::vector<uint32_t> items;
                    if (!r.list_items(*bp, &items) || items.size() != 32) continue;
                    bool ok = true;
                    for (int j = 0; j < 32 && ok; ++j) {
                        const int row = j / 8, col = (j / 2) % 4;
                        const double v = (j & 1) ? double(row) / 3.0 : double(col) / 3.0;
                        std::string e2;
                        if (!write_real(r, items[size_t(j)], v, &e2)) ok = false;
                    }
                    if (ok) { ++ctx->grids; ctx->items += 32; }
                    else ++ctx->skipped;
                }
            }
        }
        for (size_t i = 0; i < vals.size(); ++i) ident_walk(r, vals[i], ctx, depth + 1, err);
    } else if (k == psb::Kind::List) {
        std::vector<uint32_t> items;
        if (r.list_items(off, &items))
            for (uint32_t v : items) ident_walk(r, v, ctx, depth + 1, err);
    }
}

int cmd_patch_identity(const std::string& in, const std::string& out,
                      const std::string& keep) {
    std::vector<uint8_t> data;
    if (!read_file(in, &data)) { std::fprintf(stderr, "cannot read %s\n", in.c_str()); return 1; }
    psb::PsbReader r;
    std::string err;
    if (!r.load(data, &err)) { std::fprintf(stderr, "load: %s\n", err.c_str()); return 1; }
    IdentCtx ctx;
    ctx.keep = keep;
    if (!keep.empty() && keep[0] == '#') { ctx.keepIndex = std::atoi(keep.c_str() + 1); ctx.keep.clear(); }
    ident_walk(r, r.root_offset(), &ctx, 0, &err);
    if (ctx.keepIndex >= 0 || !ctx.keep.empty())
        std::printf("kept %d node(s) matching '%s'\n", ctx.matched, keep.c_str());
    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
    auto& bytes = r.mutable_bytes();
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    std::printf("identity-patched %d grid(s) / %d values, %d non-numeric grid(s) skipped => %s\n",
                ctx.grids, ctx.items, ctx.skipped, out.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// node tree: the CHILD ORDER that a meshSyncChildMask bit is supposed to index
// ---------------------------------------------------------------------------
void nodetree_rec(const emote::EmoteMotion& m, int idx, int depth, const std::string& filter,
                  const std::string& motion, int* shown, int only_idx = -1) {
    if (idx < 0 || idx >= int(m.nodes.size()) || depth > 64) return;
    const auto& n = m.nodes[idx];
    const bool hit = only_idx >= 0 ? (idx == only_idx)
                                   : (filter.empty() || n.label.find(filter) != std::string::npos);
    if (!hit) {
        // filtered: still walk, a matching node can sit below a non-matching one
        for (int ch : n.children) nodetree_rec(m, ch, depth + 1, filter, motion, shown, only_idx);
        return;
    }
    std::string region;
    for (const auto& fr : n.frames)
        if (fr.hasBp || fr.src == "blank") { region = fr.icon; break; }
    std::string kids;
    for (size_t i = 0; i < n.children.size() && i < 12; ++i) {
        kids += (i ? "," : "");
        kids += std::to_string(n.children[i]);
    }
    if (n.children.size() > 12) kids += ",...";
    std::string reg = region.empty() ? std::string("-") : region;
    std::printf("%-14s %*s[%d] %-22s type=%d tf=%d mc=%d sync=0x%-4X div=%-2d inh=0x%X "
                "nch=%zu ch=[%s] reg=%s\n",
                motion.c_str(), depth * 2, "", idx, n.label.c_str(), n.type, n.meshTransform,
                n.meshCombine, n.meshSyncChildMask, n.meshDivision, n.inheritMask,
                n.children.size(), kids.c_str(), reg.c_str());
    if (++*shown >= 4000) return;
    for (int ch : n.children) nodetree_rec(m, ch, depth + 1, filter, motion, shown, only_idx);
}

int cmd_nodetree(const std::string& path, const std::string& motion_filter,
                 const std::string& label_filter) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, &bytes)) { std::fprintf(stderr, "cannot read: %s\n", path.c_str()); return 1; }
    emote::EmoteFile ef;
    std::string err;
    if (!ef.load(bytes, &err)) { std::fprintf(stderr, "load: %s\n", err.c_str()); return 1; }
    int shown = 0;
    int only_idx = -1;
    if (!label_filter.empty() && label_filter[0] == '#') only_idx = std::atoi(label_filter.c_str() + 1);
    for (const auto& obj : ef.objects) {
        for (const auto& [mname, midx] : obj.motions) {
            if (midx < 0 || midx >= int(ef.motions.size())) continue;
            if (!motion_filter.empty() && mname.find(motion_filter) == std::string::npos) continue;
            const auto& m = ef.motions[size_t(midx)];
            if (m.nodes.empty()) continue;
            std::printf("--- object '%s' motion '%s' nodes=%zu roots=%zu ---\n",
                        obj.name.c_str(), mname.c_str(), m.nodes.size(), m.layer.size());
            for (int root : m.layer) nodetree_rec(m, root, 0, label_filter, mname, &shown, only_idx);
        }
    }
    std::fprintf(stderr, "%d node(s) printed\n", shown);
    return 0;
}

// ---------------------------------------------------------------------------
// patch-int: rewrite a numeric object member in place (same width).
// The differential fixture for meshSyncChildMask: same PSB, only that value
// differs, so the official driver's render isolates its effect.
// ---------------------------------------------------------------------------
// NumberN0 (0x04) is the sentinel; 0x05..0x0C store 1..8 bytes of little-endian.
constexpr uint8_t kNumFirst = 0x05, kNumLast = 0x0C;
inline int num_width(psb::Kind k) { return int(uint8_t(k)) - int(kNumFirst) + 1; }

bool write_number(psb::PsbReader& r, uint32_t off, int64_t value, std::string* err) {
    const psb::Kind k = r.kind_at(off);
    if (uint8_t(k) < kNumFirst || uint8_t(k) > kNumLast) {
        *err = "not a number (kind=0x" + std::to_string(int(uint8_t(k))) + ")";
        return false;
    }
    const int w = num_width(k);
    const uint64_t uv = uint64_t(value);
    if (w < 8 && (uv >> (8 * w)) != 0) {
        *err = "value does not fit the stored width (" + std::to_string(w) + " byte(s))";
        return false;
    }
    auto& b = r.mutable_bytes();
    for (int i = 0; i < w; ++i) b[size_t(off) + 1 + size_t(i)] = uint8_t((uv >> (8 * i)) & 0xff);
    return true;
}

struct IntCtx {
    std::string label, member;
    int64_t value = 0;
    int nth = -1;   // patch only the nth match (0-based); -1 = all
    int seen = 0;
    int patched = 0;
    std::string width_note;
};

void int_walk(psb::PsbReader& r, uint32_t off, IntCtx* ctx, int depth, std::string* err) {
    if (depth > 96 || !err->empty()) return;
    const psb::Kind k = r.kind_at(off);
    if (k == psb::Kind::Objects) {
        std::vector<std::string> keys;
        std::vector<uint32_t> vals;
        if (!r.object_entries(off, &keys, &vals)) return;
        std::string label;
        int64_t label_idx = -1;
        uint32_t target = 0;
        bool has = false;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == "label") {
                if (auto s = r.read_string(vals[i])) label = *s;
                // raw pool index: lets a non-ASCII label be named as "#427" on
                // a command line whose code page cannot carry it.
                const psb::Kind lk = r.kind_at(vals[i]);
                if (uint8_t(lk) >= 0x15 && uint8_t(lk) <= 0x18) {
                    const int w = int(uint8_t(lk)) - 0x15 + 1;
                    int64_t raw = 0;
                    for (int b = 0; b < w; ++b)
                        raw |= int64_t(r.mutable_bytes()[size_t(vals[i]) + 1 + size_t(b)]) << (8 * b);
                    label_idx = raw;
                }
            } else if (keys[i] == ctx->member) {
                target = vals[i];
                has = true;
            }
        }
        bool label_hit = false;
        if (ctx->label == "-") label_hit = true;
        else if (!ctx->label.empty() && ctx->label[0] == '#')
            label_hit = (label_idx == std::atoll(ctx->label.c_str() + 1));
        else
            label_hit = (label == ctx->label);
        if (has && label_hit) {
            const int idx = ctx->seen++;
            if (ctx->nth < 0 || ctx->nth == idx) {
                const psb::Kind vk = r.kind_at(target);
                const int w = num_width(vk);
                if (!write_number(r, target, ctx->value, err)) return;
                ++ctx->patched;
                if (ctx->width_note.empty())
                    ctx->width_note = std::to_string(w) + " byte(s) stored";
            }
        }
        for (size_t i = 0; i < vals.size(); ++i) int_walk(r, vals[i], ctx, depth + 1, err);
    } else if (k == psb::Kind::List) {
        std::vector<uint32_t> items;
        if (r.list_items(off, &items))
            for (uint32_t v : items) int_walk(r, v, ctx, depth + 1, err);
    }
}

int cmd_patch_int(const std::string& in, const std::string& out, const std::string& label,
                  const std::string& member, int64_t value, int nth) {
    std::vector<uint8_t> data;
    if (!read_file(in, &data)) { std::fprintf(stderr, "cannot read %s\n", in.c_str()); return 1; }
    psb::PsbReader r;
    std::string err;
    if (!r.load(data, &err)) { std::fprintf(stderr, "load: %s\n", err.c_str()); return 1; }
    IntCtx ctx;
    ctx.label = label;
    ctx.member = member;
    ctx.value = value;
    ctx.nth = nth;
    int_walk(r, r.root_offset(), &ctx, 0, &err);
    if (!err.empty()) { std::fprintf(stderr, "patch failed: %s\n", err.c_str()); return 1; }
    if (ctx.patched == 0) {
        std::fprintf(stderr, "no object matched (label='%s', member='%s', candidates=%d)\n",
                     label.c_str(), member.c_str(), ctx.seen);
        return 1;
    }
    if (!write_file(out, r.mutable_bytes())) {
        std::fprintf(stderr, "cannot write %s\n", out.c_str());
        return 1;
    }
    std::printf("patched %d/%d '%s'.%s = %lld (%s) => %s\n", ctx.patched, ctx.seen,
                label.c_str(), member.c_str(), (long long)value, ctx.width_note.c_str(),
                out.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage();
        return 2;
    }
    const std::string cmd = argv[1];
    const std::string path = argv[2];
    try {
        if (cmd == "info") return cmd_info(path);
        if (cmd == "json") {
            if (argc > 3) {
                const std::string out = argv[3];
                return cmd_json(path, &out);
            }
            return cmd_json(path, nullptr);
        }
        if (cmd == "tree") {
            int depth = 3;
            size_t max_lines = 4000;
            for (int i = 3; i + 1 < argc; ++i) {
                if (std::strcmp(argv[i], "--depth") == 0) depth = std::atoi(argv[i + 1]);
                else if (std::strcmp(argv[i], "--max") == 0)
                    max_lines = size_t(std::atoll(argv[i + 1]));
            }
            return cmd_tree(path, depth, max_lines);
        }
        if (cmd == "patch-motion") {
            if (argc < 7) {
                std::fprintf(stderr,
                    "usage: psb patch-motion <in.psb> <out.psb> <node-label> <old-motion> <new-motion>\n");
                return 2;
            }
            return cmd_patch_motion(path, argv[3], argv[4], argv[5], argv[6]);
        }
        if (cmd == "strings") return cmd_strings(path);
        if (cmd == "nodetree")
            return cmd_nodetree(path, argc > 3 ? argv[3] : "", argc > 4 ? argv[4] : "");
        if (cmd == "patch-int") {
            if (argc < 7) {
                std::fprintf(stderr,
                    "usage: psb patch-int <in.psb> <out.psb> <label> <member> <value> [nth]\n");
                return 2;
            }
            return cmd_patch_int(path, argv[3], argv[4], argv[5], std::atoll(argv[6]),
                                 argc > 7 ? std::atoi(argv[7]) : -1);
        }
        if (cmd == "patch-identity") {
            if (argc < 4) {
                std::fprintf(stderr, "usage: psb patch-identity <in.psb> <out.psb> [keep-label|#index]\n");
                return 2;
            }
            return cmd_patch_identity(path, argv[3], argc > 4 ? argv[4] : "");
        }
        if (cmd == "timelines") return cmd_timelines(path, argc > 3 ? argv[3] : "");
        if (cmd == "controls") return cmd_controls(path);
        if (cmd == "iconpng") {
            if (argc < 4) { print_usage(); return 2; }
            return cmd_iconpng(path, argv[3]);
        }
        if (cmd == "unpack") {
            if (argc < 4) { print_usage(); return 2; }
            return cmd_unpack(path, argv[3]);
        }
        if (cmd == "motions") return cmd_motions(path);
        if (cmd == "sources") return cmd_sources(path);
        if (cmd == "icons") return cmd_icons(path, argc > 3 ? argv[3] : "");
        if (cmd == "extract") {
            if (argc < 4) {
                print_usage();
                return 2;
            }
            return cmd_extract(path, argv[3]);
        }
        if (cmd == "render") {
            if (argc < 4) {
                print_usage();
                return 2;
            }
            int w = 1920, h = 1080;
            double scale = 1.0;
            std::map<std::string, double> vars;
            for (int i = 4; i < argc; ++i) {
                if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
                    const std::string s = argv[++i];
                    const size_t x = s.find('x');
                    if (x != std::string::npos) {
                        w = std::atoi(s.substr(0, x).c_str());
                        h = std::atoi(s.substr(x + 1).c_str());
                    }
                } else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
                    scale = std::atof(argv[++i]);
                } else if (std::strcmp(argv[i], "--var") == 0 && i + 1 < argc) {
                    const std::string kv = argv[++i];
                    const size_t eq = kv.find('=');
                    if (eq != std::string::npos)
                        vars[kv.substr(0, eq)] = std::atof(kv.substr(eq + 1).c_str());
                }
            }
            if (w <= 0 || h <= 0) {
                std::fprintf(stderr, "--size must be WxH with positive values\n");
                return 2;
            }
            return cmd_render(path, argv[3], w, h, scale, vars);
        }
        print_usage();
        return 2;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "psb: %s\n", ex.what());
        return 1;
    }
}
