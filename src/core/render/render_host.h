#pragma once
// RenderHost: the small runtime-side surface render depends on. Keeping this
// here lets renderer.h/font.h avoid including runtime.h.
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace oa::render {

class Transition;

class RenderHost {
public:
    virtual ~RenderHost() = default;

    virtual int stage_width() const = 0;
    virtual int stage_height() const = 0;
    virtual std::string resolve_magic_path(const std::string& name) const = 0;
    virtual std::optional<std::string> host_variable(const std::string& key) const = 0;
    virtual std::optional<std::vector<uint8_t>> read_save_file(
        const std::string& rel_path) const = 0;
    virtual uint64_t now_ms() const = 0;
    virtual void post_frame_capture() = 0;
    virtual void transition_begin(
        const std::map<std::string, std::string>& params) = 0;
    /// Compatibility adapters share the runtime-owned transition state.
    virtual void bind_text_metrics(
        std::function<std::tuple<double, double, double>()> fn) = 0;
};

} // namespace oa::render
