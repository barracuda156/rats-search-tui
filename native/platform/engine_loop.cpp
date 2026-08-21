#include "platform/engine_loop.h"

#include <algorithm>

namespace ratsn::platform {

namespace {
constexpr auto kMaxPoll = std::chrono::milliseconds(200);

// std::vector used as a min-heap (soonest `when` on top) via std::push_heap /
// std::pop_heap: std::make_heap's default comparator builds a max-heap, so
// "less" here is defined as "later" to put the earliest deadline on top.
bool laterThan(const EngineLoop::DelayedTask& a, const EngineLoop::DelayedTask& b)
{
    return a.when > b.when;
}
} // namespace

void EngineLoop::post(Task task)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void EngineLoop::postDelayed(Task task, int delay_ms)
{
    const auto when = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        delayed_.push_back(DelayedTask { when, std::move(task) });
        std::push_heap(delayed_.begin(), delayed_.end(), laterThan);
    }
    cv_.notify_one();
}

std::vector<EngineLoop::Task> EngineLoop::collectDueTasks()
{
    std::vector<Task> due;
    due.reserve(queue_.size());
    while (!queue_.empty()) {
        due.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }

    const auto now = std::chrono::steady_clock::now();
    while (!delayed_.empty() && delayed_.front().when <= now) {
        std::pop_heap(delayed_.begin(), delayed_.end(), laterThan);
        due.push_back(std::move(delayed_.back().task));
        delayed_.pop_back();
    }
    return due;
}

void EngineLoop::run()
{
    running_.store(true, std::memory_order_relaxed);
    while (!stopRequested_.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lock(mutex_);

        auto waitFor = kMaxPoll;
        if (!delayed_.empty()) {
            const auto untilNext = delayed_.front().when - std::chrono::steady_clock::now();
            waitFor = std::clamp(std::chrono::duration_cast<std::chrono::milliseconds>(untilNext),
                std::chrono::milliseconds(0), kMaxPoll);
        }
        if (queue_.empty() && waitFor > std::chrono::milliseconds(0)) {
            cv_.wait_for(lock, waitFor, [this] {
                return !queue_.empty() || stopRequested_.load(std::memory_order_relaxed);
            });
        }

        std::vector<Task> due = collectDueTasks();
        lock.unlock();

        for (Task& task : due)
            task();
    }
    running_.store(false, std::memory_order_relaxed);
}

void EngineLoop::stop()
{
    stopRequested_.store(true, std::memory_order_relaxed);
    cv_.notify_all();
}

} // namespace ratsn::platform
