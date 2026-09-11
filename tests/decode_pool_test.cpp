// DecodePool unit tests: configurable worker counts, long-job slot
// capacity, clean stop. Plain asserts; return 0 on success.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "core/media/decode_pool.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
} // namespace

int main() {
    try {
        {
            oa::media::DecodePool zero(0);
            check(zero.threads() == 0, "0 threads = disabled pool");
            check(!zero.submit_long([] {}), "disabled pool rejects jobs");
        }
        {
            oa::media::DecodePool pool(2);
            check(pool.threads() == 2, "two workers");
            std::atomic<int> ran{0};
            check(pool.submit_long([&] { ++ran; }), "long job accepted");
            // Two long slots are occupied while the first job still runs? No:
            // the first job finishes immediately; capacity frees on return.
            while (ran.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            check(ran.load() == 1, "job executed");
        }
        {
            // Capacity: with one worker, only one long job may be in flight.
            oa::media::DecodePool pool(1);
            std::atomic<bool> release{false};
            std::atomic<bool> started{false};
            check(pool.submit_long([&] {
                      started.store(true);
                      while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                  }),
                  "first long job holds the only slot");
            while (!started.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            check(!pool.submit_long([] {}), "second long job rejected (slot busy)");
            release.store(true);
            while (!pool.submit_long([] {})) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            // pool.stop() must join without hanging or throwing; a stuck join
            // trips the ctest timeout, so no separate assertion is needed.
            pool.stop();
        }
        {
            // Configurable via the environment default too (constructor -1).
            oa::media::DecodePool pool(-1);
            check(pool.threads() >= 0 && pool.threads() <= 32, "auto thread count sane");
            pool.stop();
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "EXC: %s\n", ex.what());
        return 1;
    }
    if (failures) {
        std::fprintf(stderr, "decode_pool_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("decode_pool_test: all ok\n");
    return 0;
}
