#pragma once
// Media decode thread pool (audio/video decode + media tick off the main thread). Design constraints: * thread COUNT is fully controllable...
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace oa::media {

class DecodePool {
public:
    /// threads < 0: pick from OA_DECODE_THREADS / platform default.
    explicit DecodePool(int threads = -1);
    ~DecodePool();

    DecodePool(const DecodePool&) = delete;
    DecodePool& operator=(const DecodePool&) = delete;

    /// Number of worker threads actually running (0 = disabled).
    int threads() const { return threads_; }

    /// Run `job` on the pool when a worker slot is free. Long-lived jobs
    /// (per-channel decode loops) must call this and keep their slot until
    /// done; returns false when every worker is already busy, in which case
    /// the caller runs its synchronous fallback instead.
    bool submit_long(std::function<void()> job);

    /// Request every job to stop and join the workers. Long jobs must poll
    /// stop_requested()/respect their own cancel flag to exit promptly.
    void stop();

private:
    struct Job {
        std::function<void()> fn;
        bool is_long = false;
        std::atomic<bool>* slot = nullptr; // capacity token to release
    };

    int threads_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<int> slots_free_{0};
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<Job> queue_;
    std::vector<std::thread> workers_;

    void worker_main();
};

} // namespace oa::media
