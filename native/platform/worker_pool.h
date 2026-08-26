#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ratsn::platform {

// Fixed-size pool of worker threads draining a FIFO task queue -- the native
// stand-in for Qt's QThreadPool (docs/M8-PLAN.md deviation #1), used by the
// tracker scrapers (engine/swarm_scraper.cpp, engine/site_scraper.cpp) to run
// blocking network calls (tracker announces, HTTP fetches) off the EngineLoop
// thread. Threads are I/O-bound, not CPU-bound, so a pool sized above the
// core count is fine -- it just bounds how many sockets wait at once.
class WorkerPool {
public:
    explicit WorkerPool(int threadCount);
    // Joins every worker; safe even if stopAndDrain() was never called.
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    // Enqueues a task. Dropped silently once stopAndDrain() has been called.
    void post(std::function<void()> task);

    // Drops queued-but-not-started tasks, then waits for whatever is already
    // running to finish -- Qt's threadPool_.clear() + waitForDone() pair.
    // Idempotent; safe to call from the destructor.
    void stopAndDrain();

private:
    void workerLoop();

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
};

} // namespace ratsn::platform
