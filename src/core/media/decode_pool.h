#pragma once
// Media decode thread pool (audio/video decode + media tick off the main
// thread). Design constraints:
//
//   * thread COUNT is fully controllable: constructor argument wins, then
//     the OA_DECODE_THREADS env var, then a platform default (wasm defaults
//     to 1 — the common case — but is NOT hard-capped: builds with
//     pthread/shared memory can set 2+; native defaults to
//     min(4, hardware_concurrency)).
//   * worker code is allocation-light (chunk buffers are owned by the
//     callers) so wasm heaps do not churn per job.
//   * long-lived jobs (one streaming decode loop per channel) occupy a
//     worker slot; submit_long() only succeeds while a slot is free, so
//     callers can fall back to the deterministic synchronous path instead
//     of queuing behind an unbounded backlog.
//
// The pool never changes WHAT is decoded, only WHERE the CPU time is spent:
// worker output is a byte-identical prefetch of the same deterministic
// source-frame sequence the synchronous path would produce.
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
