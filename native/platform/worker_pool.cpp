#include "platform/worker_pool.h"

namespace ratsn::platform {

WorkerPool::WorkerPool(int threadCount)
{
    workers_.reserve(static_cast<size_t>(threadCount));
    for (int i = 0; i < threadCount; ++i)
        workers_.emplace_back([this] { workerLoop(); });
}

WorkerPool::~WorkerPool()
{
    stopAndDrain();
}

void WorkerPool::post(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return;
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void WorkerPool::stopAndDrain()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return;
        stopping_ = true;
        queue_.clear(); // drop tasks that never started
    }
    cv_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable())
            worker.join();
    }
    workers_.clear();
}

void WorkerPool::workerLoop()
{
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                // Only reachable with stopping_ true (the wait predicate),
                // and nothing left to run.
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
    }
}

} // namespace ratsn::platform
