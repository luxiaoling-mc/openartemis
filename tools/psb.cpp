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
        "usage: psb <info|tree|json|motions|sources|icons|extract|render> <file.psb> [args]\n"
        "  psb info    <file.psb>\n"
        "  psb tree    <file.psb> [--depth N] [--max N]     container structure\n"
        "  psb json    <file.psb> [out.json]                raw tree as JSON\n"
        "  psb motions <file.psb>                           objects/motions/nodes\n"
        "  psb sources <file.psb>                           atlas sources (type/size)\n"
        "  psb icons   <file.psb> [source]                  icon rects/origins\n"
        "  psb extract <file.psb> <outdir>                  atlas PNGs + manifest\n"
        "  psb render  <file.psb> <out.png> [--size WxH] [--scale S] [--var k=v]\n"
        "      Static (tick-0) pose render; --var sets E-mote variable values\n"
        "      (e.g. --var face_talk=1 --var face_eye_open=0).\n");
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
