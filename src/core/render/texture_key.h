#pragma once
// oa::render — 资源读取域键。 早期模型把"这一层的内容怎么读"编码进 Layer::file 的字符串拼写 —— 宿主上传 的视频帧/emote 画布占用保留命名空间 `__video_layer__:<id>` / `__emote_layer__:<id>`...
#include <cstdint>
#include <string>
#include <utility>

namespace oa::render {

struct TextureKey {
    enum class Domain : uint8_t {
        Asset,        // path+file:文件系统资源(解码资产)
        VideoFrame,   // 宿主上传视频帧(名 = 层 id)
        EmoteCanvas,  // 宿主 emote 画布(名 = 层 id)
        OverlayFrame, // 全舞台 overlay 帧(名 = kOverlayNodeId)
    };

    Domain domain = Domain::Asset;
    std::string name;

    bool empty() const { return name.empty(); }
    bool is_asset() const { return domain == Domain::Asset; }

    bool operator<(const TextureKey& o) const {
        if (domain != o.domain) return uint8_t(domain) < uint8_t(o.domain);
        return name < o.name;
    }
    bool operator==(const TextureKey& o) const {
        return domain == o.domain && name == o.name;
    }
    bool operator!=(const TextureKey& o) const { return !(*this == o); }
};

/// 资源文件键(Asset 域;名 = path + file,原公式原样保留)。
inline TextureKey asset_key(std::string name) {
    return TextureKey{TextureKey::Domain::Asset, std::move(name)};
}

} // namespace oa::render
