#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ratsn::platform {
class EngineLoop;
class WorkerPool;
}

namespace ratsn::engine {

// Announces to UDP/HTTP BitTorrent trackers to read a torrent's swarm counts
// (seeders/leechers/completed). Port of src/net/swarm_scraper.{h,cpp}
// (docs/M8-PLAN.md item 1). Not to be confused with TrackerSiteScraper, which
// scrapes tracker *websites* for poster/description metadata.
//
// Pure transport-side helper: it only talks to trackers and reports results —
// it never touches the index. TrackerService listens for the result and
// persists it.
//
// EngineLoop-confined (docs/M8-PLAN.md deviation #1): requestScrape()/stop()
// are only ever called from the engine thread (Indexer's onIndexed hook, TUI
// posts), so the cooldown map / pending queue / active count need no mutex —
// unlike Qt's three, whose callers can arrive on arbitrary threads. The
// blocking per-tracker announces still run off-thread on a dedicated
// WorkerPool; each one's completion is posted back onto the engine thread.
class SwarmScraper {
public:
    using ScrapedCallback
        = std::function<void(const std::string& infoHash, int seeders, int leechers, int completed)>;

    explicit SwarmScraper(platform::EngineLoop& engineLoop);
    ~SwarmScraper();

    SwarmScraper(const SwarmScraper&) = delete;
    SwarmScraper& operator=(const SwarmScraper&) = delete;

    // Called once, on success, with the best swarm counts seen across the
    // torrent's trackers -- not called when every tracker fails. Runs on the
    // engine thread. Set before the first requestScrape().
    void setScrapedCallback(ScrapedCallback callback) { scraped_ = std::move(callback); }

    // Request a scrape for a torrent. `infoHash` must be a 40-char hex
    // string. If `trackers` is empty the built-in default list is used.
    // Non-blocking. Must be called on the engine thread; duplicate requests
    // within the cooldown window are dropped.
    void requestScrape(const std::string& infoHash, const std::vector<std::string>& trackers = {});

    // Stop scraping: reject further requests, drop the queue, and wait for
    // in-flight announces to drain so no pool task outlives this object.
    // Idempotent; must be called on the engine thread, before destruction.
    void stop();

    static constexpr int kTimeoutMs = 15000; // 15 s per announce
    static constexpr int kCheckIntervalSecs = 300; // 5 min per-hash cooldown
    static constexpr int kMaxConcurrent = 5; // concurrent scrapes

private:
    struct PendingRequest {
        std::string infoHash;
        std::vector<std::string> trackers;
    };

    // Drop cooldown entries older than the cooldown window so recentChecks_
    // cannot grow without bound over long uptimes.
    void pruneStaleChecks(std::chrono::steady_clock::time_point now);
    // Fan out to `trackers`, keep the best successful result, then report.
    void startScrape(const std::string& infoHash, const std::vector<std::string>& trackers);
    // Drain the overflow queue: start as many queued scrapes as free slots
    // allow. Called whenever a slot frees up (a hash finished) -- replaces
    // Qt's 500ms poll timer (docs/M8-PLAN.md deviation #1): same effect,
    // since freeing is exactly when there's anything new to start.
    void processQueue();

    static constexpr int kInfoHashHexLength = 40; // 20-byte hash as hex
    static constexpr int kMaxPoolThreads = 16; // announce worker threads

    platform::EngineLoop& engineLoop_;
    ScrapedCallback scraped_;

    // Per-hash cooldown bookkeeping, pruned periodically in pruneStaleChecks().
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> recentChecks_;
    std::chrono::steady_clock::time_point lastPrune_ {};
    bool havePruned_ = false;

    // Overflow queue used when kMaxConcurrent scrapes are already in flight.
    std::deque<PendingRequest> pendingQueue_;
    int activeRequests_ = 0;

    // Dedicated pool for the blocking tracker announces so stop() can drain
    // it deterministically.
    std::unique_ptr<platform::WorkerPool> pool_;

    // Set true by stop(): gates new requests and makes in-flight announce
    // loops bail early instead of hammering every remaining tracker. Read
    // from pool worker threads too, so it stays atomic even though stop()
    // itself only ever runs on the engine thread.
    std::atomic<bool> stopping_ { false };
};

} // namespace ratsn::engine
