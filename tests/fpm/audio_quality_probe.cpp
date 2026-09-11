// research/125 diagnostic (report-only; NOT a ctest): drives ONE real BGM
// asset through the engine's real decode + mix chain with a virtual tick
// clock and no audio device, so the samples the mixer produces can be written
// to disk (OA_AUDIO_REC=<prefix>) and compared OFFLINE against the material:
//
//   * the source ogg decoded + resampled with a reference resampler (soxr),
//   * the same asset at the engine's output rate (no resampling at all).
//
// No device => no research/118 pacer and no SDL stream: the recorded
// <prefix>.mix.wav is exactly the producer's signal face (BGM/SE/voice sum),
// which is the right arm for format/resampling/fade questions. The device
// face (underrun dropouts) is recorded by a device run of the same probe
// (drop SDL_AUDIODRIVER=dummy / leave OA_AQ_DEVICE=1) via SDL's post-mix hook.
//
// Env:
//   OA_AQ_ASSET   raw .ogg path on disk (required)
//   OA_AQ_NAME    logical name handed to the engine (default: the basename)
//   OA_AQ_TICK_MS tick delta in ms (default 16.667 = 60 fps)
//   OA_AQ_SECONDS run length (default 20)
//   OA_AQ_FADE_MS BGM fade-in (default 0 = raw passthrough for A/B maths)
//   OA_AQ_GAIN    raw gain 0-1000 (default 1000)
//   OA_AQ_VOL     bgm bus volume (default 1.0)
//   OA_AQ_LOOP    1/0 loop the asset (default 1)
//   OA_AQ_DEVICE  1 = open a real audio device (default 0: silent, no pacer)
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "SDL3/SDL.h"

#include "core/media/audio.h"

namespace {

std::optional<std::vector<uint8_t>> read_file(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return std::nullopt;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        return std::nullopt;
    }
    std::vector<uint8_t> bytes;
    bytes.resize(size_t(size));
    const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (got != bytes.size()) return std::nullopt;
    return bytes;
}

const char* env_or(const char* key, const char* fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? v : fallback;
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string asset = env_or("OA_AQ_ASSET", "");
    if (asset.empty()) {
        std::printf("[aq] OA_AQ_ASSET=<raw .ogg path> is required\n");
        return 2;
    }
    std::string name = env_or("OA_AQ_NAME", "");
    if (name.empty()) {
        const size_t slash = asset.find_last_of("/\\");
        name = slash == std::string::npos ? asset : asset.substr(slash + 1);
    }
    const double tick_ms = std::atof(env_or("OA_AQ_TICK_MS", "16.667"));
    const double seconds = std::atof(env_or("OA_AQ_SECONDS", "20"));
    const uint64_t fade_ms = uint64_t(std::atoll(env_or("OA_AQ_FADE_MS", "0")));
    const int gain = std::atoi(env_or("OA_AQ_GAIN", "1000"));
    const float vol = float(std::atof(env_or("OA_AQ_VOL", "1.0")));
    const bool loop = std::atoi(env_or("OA_AQ_LOOP", "1")) != 0;
    const bool use_device = std::atoi(env_or("OA_AQ_DEVICE", "0")) != 0;

    oa::media::MediaPlayers mp;
    oa::media::AudioEngine ae;
    mp.set_loader([&](const std::string& logical) { return read_file(asset); });
    if (use_device) {
        if (!mp.init()) {
            std::printf("[aq] no audio device available\n");
            return 77;
        }
        std::printf("[aq] device driver=%s mix_target=%zu frames\n",
                    SDL_GetCurrentAudioDriver(), mp.mix_target_frames());
    }
    ae.set_master_volume(1.0f);
    ae.set_bgm_volume(vol);
    ae.set_se_volume(vol);

    oa::media::BgmConfig cfg;
    cfg.loop_play = loop;
    cfg.gain = gain;
    cfg.fade_in_ms = fade_ms;
    ae.play_bgm(name, cfg);

    const uint64_t ticks = uint64_t(seconds * 1000.0 / tick_ms);
    uint64_t emitted_ms = 0;
    // Device arm: pace the ticks on the wall clock (the app's cadence, which is
    // what the 118 pacer and the device see). Silent arm: as fast as possible
    // (virtual clock; the recorded stream is the pure producer signal).
    auto next = std::chrono::steady_clock::now();
    const auto t0 = next;
    for (uint64_t i = 0; i < ticks; ++i) {
        const uint64_t target = uint64_t(std::llround(double(i + 1) * tick_ms));
        const uint64_t delta = target > emitted_ms ? target - emitted_ms : 1;
        emitted_ms = target;
        if (use_device) {
            next += std::chrono::microseconds(delta * 1000);
            std::this_thread::sleep_until(next);
        }
        ae.update(delta);
        mp.update(delta, ae, nullptr);
    }
    const double wall_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[aq] asset=%s ticks=%llu fade=%llums gain=%d loop=%d "
                "bgm_playing=%d players=%zu wall=%.2fs refill=%d target=%zu "
                "paced_out=%llu refill_frames=%llu callbacks=%llu underruns=%llu\n",
                name.c_str(), (unsigned long long)ticks, (unsigned long long)fade_ms,
                gain, loop ? 1 : 0, ae.is_bgm_playing() ? 1 : 0, mp.active_players(),
                wall_s, mp.mix_refill_enabled() ? 1 : 0, mp.mix_target_frames(),
                (unsigned long long)mp.paced_out_ticks(),
                (unsigned long long)mp.refill_frames(), mp.device_callbacks(),
                (unsigned long long)mp.device_underruns(0));
    mp.release();
    return 0;
}
