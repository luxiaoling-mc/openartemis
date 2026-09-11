#pragma once
// oa::render — 资源读取域键。
//
// 早期模型把"这一层的内容怎么读"编码进 Layer::file 的字符串拼写 —— 宿主上传
// 的视频帧/emote 画布占用保留命名空间 `__video_layer__:<id>` /
// `__emote_layer__:<id>`,分类(kind_of)与纹理缓存都靠前缀嗅探区分"资源文件"
// 与"宿主供帧"。该约定有两个代价:
//   1) 分类权威依赖拼写(与"种类 = 内容角色"相悖;同形文件名会被误判/漏判,
//      测试只能锚定拼写边界);
//   2) 缓存键必须靠命名空间避免与资源路径撞名(约定性正确,而非结构性正确)。
//
// 现改由**角色实例决定读取域**:资源键 = {域, 名} 二元组,缓存按域分桶,
// 撞名在结构上不可能;"怎么读资源文件"是角色的读取面(texture_key +
// content_quad)的事,与 file 字段里写了什么无关。
//
// 域语义:
//   Asset        — 资源文件:名 = path + file,经 magic path 解析 → 解码
//                  (decoded 缓存 + mask 合成面)。
//   VideoFrame   — 该层视频通道的宿主上传帧:名 = 层 id(= 通道 id)。
//   EmoteCanvas  — 该层 emote 画布:名 = 层 id(CPU 上传帧或 GPU 合成目标)。
//   OverlayFrame — 全舞台 overlay 帧:名 = kOverlayNodeId(宿主内部面,
//                  不经脚本层,只由 overlay 结构槽消费)。
//
// 本头文件只含纯值类型(零行为、零依赖),供 content_role.h(角色读取面)、
// renderer.h(纹理缓存/上载面)、runtime.h(尺寸探测缓存)共享。
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
