#include "core/media/decode_pool.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace oa::media {

namespace {

int env_threads() {
    if (const char* e = std::getenv("OA_DECODE_THREADS"); e && *e) {
        const int v = std::atoi(e);
        if (v >= 0) return v;
    }
    return -1; // unset
}

/// Platform default: wasm commonly runs without pthreads (single decode
/// worker keeps memory flat); native uses a small multiple of the cores.
int default_threads() {
#if defined(__EMSCRIPTEN__)
    return 1;
#else
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    return int(std::min(4u, hw));
#endif
}

} // namespace

DecodePool::DecodePool(int threads) {
    if (threads < 0) threads = env_threads();
    if (threads < 0) threads = default_threads();
    threads_ = std::min(threads, 32);
    if (threads_ <= 0) return; // explicitly disabled
    slots_free_.store(threads_);
    stopping_.store(false);
    workers_.reserve(size_t(threads_));
    for (int i = 0; i < threads_; ++i) {
        try {
            workers_.emplace_back(&DecodePool::worker_main, this);
        } catch (const std::exception& ex) {
            // Thread creation failed (e.g. wasm without pthread support):
            // degrade to whatever started successfully.
            std::fprintf(stderr, "[media] decode pool: worker %d failed: %s\n", i,
                         ex.what());
            break;
        }
    }
    threads_ = int(workers_.size());
    if (threads_ == 0) {
        stopping_.store(true);
        slots_free_.store(0);
    } else {
        slots_free_.store(threads_);
    }
}

DecodePool::~DecodePool() { stop(); }

void DecodePool::stop() {
    if (workers_.empty()) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopping_.store(true);
    }
    cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    workers_.clear();
}

bool DecodePool::submit_long(std::function<void()> job) {
    if (workers_.empty() || stopping_.load()) return false;
    if (slots_free_.fetch_sub(1) <= 0) {
        slots_free_.fetch_add(1); // all workers busy: caller falls back sync
        return false;
    }
    auto* slot = new std::atomic<bool>(true);
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push(Job{std::move(job), true, slot});
    }
    cv_.notify_one();
    return true;
}

void DecodePool::worker_main() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stopping_.load() || !queue_.empty(); });
            if (queue_.empty()) {
                if (stopping_.load()) return;
                continue;
            }
            job = std::move(queue_.front());
            queue_.pop();
        }
        // Long jobs own a capacity token for their whole lifetime: they run
        // until they decide to return (their caller polls its own cancel
        // state at chunk boundaries).
        try {
            job.fn();
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "[media] decode job error: %s\n", ex.what());
        }
        if (job.slot) {
            *job.slot = false;
            delete job.slot;
            slots_free_.fetch_add(1);
        }
    }
}

} // namespace oa::media
