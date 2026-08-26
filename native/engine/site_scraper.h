#pragma once

#include "librats/util/json.h"

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

// Result of scraping a single tracker website for one info-hash. Internal
// DTO: the parse* helpers fill it in and checkAllComplete() folds the
// per-tracker DTOs into the single JSON object delivered by the scraped
// callback.
struct TrackerSiteInfo {
    std::string trackerName; // "rutracker" | "nyaa"
    std::string name; // torrent title as shown on the tracker
    std::string poster; // poster image URL
    std::string description; // plain-text description
    std::string contentCategory; // category / breadcrumb path
    int threadId = 0; // topic / view id on the tracker
    bool success = false;
};

// Scrapes tracker websites (RuTracker, Nyaa) for a torrent's poster image,
// description and category, using hand-rolled PCRE2 HTML parsing. Port of
// src/net/tracker_site_scraper.{h,cpp} (docs/M8-PLAN.md item 2). Not to be
// confused with SwarmScraper, which announces to trackers for seeder/leecher
// counts.
//
// Pure network-side helper: it NEVER touches the index. All strategies for a
// hash run in parallel; once they finish, the merged metadata is delivered
// via the scraped callback as a JSON object whose keys are those of the
// torrent's `info` field (poster, description, contentCategory, trackers[],
// rutrackerThreadId, nyaaThreadId, trackerName). TrackerService listens for
// it and persists those fields onto the torrent.
//
// EngineLoop-confined (docs/M8-PLAN.md deviation #1): scrape()/stop() only
// run on the engine thread, so the cooldown map / pending-scrape bookkeeping
// / concurrency-slot count need no mutex. HTTP fetches (libcurl, blocking)
// run on a dedicated WorkerPool -- the forced translation for Qt's async
// QNetworkAccessManager, which has no native equivalent (deviation #3); each
// fetch's completion is posted back onto the engine thread before it touches
// any of the above state.
class TrackerSiteScraper {
public:
    using ScrapedCallback = std::function<void(const std::string& infoHash, const librats::Json& info)>;

    explicit TrackerSiteScraper(platform::EngineLoop& engineLoop);
    ~TrackerSiteScraper();

    TrackerSiteScraper(const TrackerSiteScraper&) = delete;
    TrackerSiteScraper& operator=(const TrackerSiteScraper&) = delete;

    // Called once all strategies for `infoHash` have finished AND at least
    // one tracker yielded data. `info` carries only the freshly scraped keys;
    // the listener merges them into the stored torrent. Runs on the engine
    // thread. Set before the first scrape().
    void setScrapedCallback(ScrapedCallback callback) { scraped_ = std::move(callback); }

    // Scrape every supported tracker for `infoHash` (40-char hex). `name` is
    // the torrent name, carried for logging/context. Non-blocking. Must be
    // called on the engine thread; requests inside the per-hash cooldown
    // window are dropped. Whether scraping happens at all is decided by the
    // caller (TrackerService).
    void scrape(const std::string& infoHash, const std::string& name);

    // Stop scraping: reject further scrape() calls and abort every in-flight
    // HTTP fetch (via libcurl's progress callback) so shutdown is not held up
    // waiting on tracker websites, then wait for the pool to drain -- unlike
    // Qt's abort-and-forget (its finished() handlers complete asynchronously
    // later), this blocks until every in-flight fetch has actually stopped,
    // matching docs/M8-PLAN.md's "shutdown completes promptly" acceptance
    // check. Idempotent; must be called on the engine thread, before
    // destruction.
    void stop();

    static constexpr int kTimeoutMs = 20000; // 20 s per request
    static constexpr int kCooldownSecs = 3600; // 1 h per-hash cooldown

    // Concurrency cap. Each scraped hash fans out kStrategies parallel HTTP
    // fetches, and every in-flight fetch holds a fully decompressed tracker
    // page in memory. A burst of freshly-crawled torrents would otherwise
    // fire an unbounded number of these at once -- the dominant heap consumer
    // under load, worse on the retro target -- so at most kMaxConcurrent
    // hashes scrape simultaneously; the rest wait in a FIFO queue drained as
    // slots free up. Mirrors SwarmScraper's cap.
    static constexpr int kMaxConcurrent = 3; // concurrent hash scrapes

private:
    struct PendingRequest {
        std::string infoHash;
        std::string name;
    };
    struct PendingScrape {
        std::string name;
        int pendingCount = 0;
        std::vector<TrackerSiteInfo> results;
    };
    struct HttpResult {
        bool ok = false;
        std::string body;
        std::string finalUrl; // post-redirect URL, for Nyaa's search->view detection
        std::string error;
    };

    // Register a hash's pending scrape and launch its strategies. Assumes a
    // concurrency slot has already been claimed by scrape()/processQueue().
    void startScrape(const std::string& infoHash, const std::string& name);
    // Drain the overflow queue: start as many queued scrapes as free slots
    // allow. Called whenever a slot frees up (a hash finished).
    void processQueue();

    // Strategy launchers -- one network round-trip family each. Each posts
    // its blocking HTTP fetch to the pool, then marshals the parse + result
    // back onto the engine thread.
    void scrapeRutracker(const std::string& hash);
    void scrapeNyaa(const std::string& hash);
    void scrapeNyaaViewPage(const std::string& hash, const std::string& viewUrl);

    // Blocking libcurl GET; runs on a pool thread only.
    HttpResult httpGet(const std::string& url, const std::vector<std::string>& extraHeaders);

    // HTML parsers (faithful ports of the legacy regex parsing). Engine-
    // thread only (called from the strategies' completion handlers).
    TrackerSiteInfo parseRutrackerHtml(const std::string& rawData);
    TrackerSiteInfo parseNyaaSearchHtml(const std::string& rawData);
    TrackerSiteInfo parseNyaaViewHtml(const std::string& rawData);

    // Called by each strategy when it finishes; merges once all have
    // reported. Engine-thread only.
    void onStrategyComplete(const std::string& hash, const TrackerSiteInfo& info);
    void checkAllComplete(const std::string& hash);

    // Strip HTML tags / decode entities into plain text.
    static std::string stripHtml(const std::string& html);
    // Decode Windows-1251 bytes to UTF-8. RuTracker still serves this legacy
    // encoding.
    static std::string decodeWindows1251(const std::string& data);
    // Clamp a description to kMaxDescriptionLength, appending an ellipsis
    // when truncated.
    static std::string truncateDescription(const std::string& text);

    // Member-function pointer type for a parallel strategy. The strategy
    // list (kStrategies) is the single source of truth for how many results
    // a scrape waits on -- the count is derived from its size, never
    // hardcoded.
    using Strategy = void (TrackerSiteScraper::*)(const std::string&);
    static const std::vector<Strategy> kStrategies;

    // Named constants (no magic numbers in the logic below).
    static constexpr int kInfoHashHexLength = 40; // 20-byte hash as hex
    static constexpr int kEncodingSniffLength = 2000; // bytes scanned for charset
    static constexpr int kMaxDescriptionLength = 5000; // description clamp
    // kMaxConcurrent hashes (3) * kStrategies.size() (2) = 6 peak concurrent
    // fetches; a little headroom without being wasteful on the retro target.
    // No Qt equivalent -- QNetworkAccessManager is single-threaded async, so
    // it never needed a worker-thread cap (deviation #3: libcurl's blocking
    // easy interface forces one thread per in-flight fetch).
    static constexpr int kMaxPoolThreads = 8;

    platform::EngineLoop& engineLoop_;
    ScrapedCallback scraped_;

    std::unique_ptr<platform::WorkerPool> pool_;
    std::atomic<bool> stopping_ { false };

    // Per-hash cooldown bookkeeping.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> recentChecks_;

    // In-flight scrapes: accumulate per-strategy results until all report.
    std::unordered_map<std::string, PendingScrape> pendingScrapes_;

    // Concurrency cap + overflow queue (see kMaxConcurrent). A scrape holds
    // one slot from startScrape() until all its strategies report in
    // checkAllComplete().
    std::deque<PendingRequest> pendingQueue_;
    int activeRequests_ = 0;
};

} // namespace ratsn::engine
