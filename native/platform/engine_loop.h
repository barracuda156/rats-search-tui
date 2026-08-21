#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace ratsn::platform {

// The single-threaded executor all app state lives on (docs/DESIGN-native.md
// §3). librats callbacks and the UI thread only ever reach app state through
// post()/postDelayed(); everything queued here runs on whichever thread calls
// run().
class EngineLoop {
public:
    using Task = std::function<void()>;

    EngineLoop() = default;
    ~EngineLoop() = default;

    EngineLoop(const EngineLoop&) = delete;
    EngineLoop& operator=(const EngineLoop&) = delete;

    // Thread-safe: called from librats callback threads and the UI thread.
    void post(Task task);
    void postDelayed(Task task, int delay_ms);

    // Runs on the calling thread until stop() is observed. Only one thread
    // may call run() at a time.
    void run();

    // Thread-safe, idempotent. Safe to call from a plain signal handler too:
    // the flag it sets is also polled on a bounded interval by run(), so a
    // notify that a handler can't safely deliver still gets noticed promptly.
    void stop();

    bool isRunning() const { return running_.load(std::memory_order_relaxed); }

    struct DelayedTask {
        std::chrono::steady_clock::time_point when;
        Task task;
    };

private:
    // Pops every task currently due (queue_ fully, delayed_ up to now). Caller
    // must hold `mutex_`.
    std::vector<Task> collectDueTasks();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> queue_;
    std::vector<DelayedTask> delayed_; // min-heap by `when`
    std::atomic<bool> stopRequested_ { false };
    std::atomic<bool> running_ { false };
};

} // namespace ratsn::platform
