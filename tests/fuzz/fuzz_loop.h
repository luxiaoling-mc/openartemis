#pragma once
// 暴力变异循环：没有 libFuzzer 时各 fuzz 目标共用的 main。
//
// 目标只要提供一个 run_one(data,size)；语料可选（命令行第一个参数给目录），
// 否则从随机字节起步。每轮把输入先落盘（默认 oa_fuzz_last.bin），进程被 ASan
// 打死时最后那份就是复现样本。异常一律吞掉：畸形输入报错是解析器的正常行为。
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace oa::fuzz {

inline size_t env_size(const char* name, size_t dflt) {
    const char* v = std::getenv(name);
    if (!v || !*v) return dflt;
    const long long parsed = std::atoll(v);
    return parsed > 0 ? size_t(parsed) : dflt;
}

inline void mutate(std::vector<uint8_t>& buf, std::mt19937_64& rng, size_t max_size) {
    std::uniform_int_distribution<int> op_dist(0, 5);
    const int ops = 1 + int(rng() % 4);
    for (int i = 0; i < ops; ++i) {
        if (buf.empty()) buf.push_back(uint8_t(rng()));
        switch (op_dist(rng)) {
            case 0: // 翻一位
                buf[rng() % buf.size()] ^= uint8_t(1u << (rng() % 8));
                break;
            case 1: // 换一个字节
                buf[rng() % buf.size()] = uint8_t(rng());
                break;
            case 2: // 写成边界值
                buf[rng() % buf.size()] = uint8_t((rng() % 2) ? 0x00 : 0xFF);
                break;
            case 3: // 插一段
                if (buf.size() < max_size) {
                    const size_t at = rng() % (buf.size() + 1);
                    const size_t n = 1 + rng() % 8;
                    buf.insert(buf.begin() + long(at), n, uint8_t(rng()));
                }
                break;
            case 4: // 删一段
                if (buf.size() > 1) {
                    const size_t at = rng() % buf.size();
                    const size_t n = 1 + rng() % std::min<size_t>(8, buf.size() - at);
                    buf.erase(buf.begin() + long(at), buf.begin() + long(at + n));
                }
                break;
            default: // 改 4 字节字段（PSB/存档/ini 的长度字段都在小端头里）
                if (buf.size() >= 4) {
                    const size_t at = rng() % (buf.size() - 3);
                    const uint32_t v = uint32_t(rng());
                    std::memcpy(buf.data() + at, &v, 4);
                }
                break;
        }
    }
}

/// 跑 iters 轮。run_one 里抛异常也算正常（目标自己决定吞还是记录）。
template <typename RunOne>
int brute_loop(int argc, char** argv, RunOne run_one) {
    const size_t iters = env_size("OA_FUZZ_ITERS", 20000);
    const size_t max_size = env_size("OA_FUZZ_MAX_INPUT", 256 * 1024);
    const uint64_t seed = env_size("OA_FUZZ_SEED", 0x9E3779B97F4A7C15ull);
    const char* dump = std::getenv("OA_FUZZ_DUMP");
    const std::string dump_path =
        dump && *dump ? std::string(dump) : std::string("oa_fuzz_last.bin");

    std::mt19937_64 rng(seed);
    std::vector<std::vector<uint8_t>> corpus;
    if (argc > 1) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(argv[1], ec)) {
            if (!entry.is_regular_file()) continue;
            std::FILE* f = std::fopen(entry.path().string().c_str(), "rb");
            if (!f) continue;
            std::vector<uint8_t> bytes;
            uint8_t chunk[8192];
            size_t got = 0;
            while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
                bytes.insert(bytes.end(), chunk, chunk + got);
            std::fclose(f);
            if (!bytes.empty() && bytes.size() <= max_size) corpus.push_back(std::move(bytes));
        }
        std::printf("[fuzz] corpus: %zu seed(s) from %s\n", corpus.size(), argv[1]);
    }
    if (corpus.empty()) {
        std::vector<uint8_t> bytes(1 + rng() % 512);
        for (auto& b : bytes) b = uint8_t(rng());
        corpus.push_back(std::move(bytes));
    }

    // 时间预算：夜里按分钟切片用（迭代次数在慢目标上不好估）。
    const size_t seconds = env_size("OA_FUZZ_SECONDS", 0);
    const auto started = std::chrono::steady_clock::now();

    std::printf("[fuzz] iters=%zu max_input=%zu seed=%llu seconds=%zu\n", iters, max_size,
                (unsigned long long)seed, seconds);
    size_t slow = 0;
    size_t done = 0;
    for (size_t i = 0; i < iters; ++i) {
        if (seconds > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
            if (elapsed >= std::chrono::seconds(seconds).count()) break;
        }
        ++done;
        std::vector<uint8_t> input = corpus[rng() % corpus.size()];
        mutate(input, rng, max_size);
        if (std::FILE* f = std::fopen(dump_path.c_str(), "wb")) {
            std::fwrite(input.data(), 1, input.size(), f);
            std::fclose(f);
        }
        const auto t0 = std::chrono::steady_clock::now();
        run_one(input.data(), input.size());
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        if (ms > 200) {
            ++slow;
            std::printf("[fuzz] slow input #%zu: %lld ms (%zu bytes) -> %s\n", i,
                        (long long)ms, input.size(), dump_path.c_str());
        }
        if ((i + 1) % 5000 == 0) std::printf("[fuzz] %zu/%zu\n", i + 1, iters);
    }
    std::printf("[fuzz] done: %zu iters, %zu slow, no crash\n", done, slow);
    return 0;
}

} // namespace oa::fuzz
