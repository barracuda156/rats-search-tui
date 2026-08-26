#include "engine/swarm_scraper.h"

#include "platform/engine_loop.h"
#include "platform/log.h"
#include "platform/worker_pool.h"

#include "librats/bittorrent/tracker.h"
#include "librats/bittorrent/types.h"

#include <mutex>

namespace ratsn::engine {

namespace {
namespace bt = librats::bittorrent;

// Port announced to trackers. We never accept connections here, so any valid
// BitTorrent port works -- trackers only echo it back in their swarm
// accounting.
constexpr int kAnnouncePort = 6881;

// A small set of well-known public trackers, used when a caller supplies
// none. The librats core bundles no default list, so we keep one here (port
// of src/net/swarm_scraper.cpp's kDefaultTrackers, verbatim).
const std::vector<std::string> kDefaultTrackers = {
    "udp://tracker.opentrackr.org:1337/announce",
    "udp://open.tracker.cl:1337/announce",
    "udp://tracker.openbittorrent.com:6969/announce",
    "udp://exodus.desync.com:6969/announce",
    "udp://tracker.torrent.eu.org:451/announce",
    "udp://open.demonii.com:1337/announce",
    "udp://explodie.org:6969/announce",
    "udp://tracker.moeking.me:6969/announce",
};

// Outcome of a single tracker announce.
struct AnnounceResult {
    int seeders = 0;
    int leechers = 0;
    int completed = 0;
    bool success = false;
    std::string error;
};

// Shared state for one hash's parallel tracker announces. Each tracker runs
// as its own pool task; they merge into `best` under `mutex` and the task
// that drops `remaining` to zero reports the aggregate back on the engine
// thread.
struct ScrapeState {
    std::mutex mutex;
    AnnounceResult best;
    std::atomic<int> remaining { 0 };
};

// Announce to a single tracker to read the torrent's seeder/leecher counts.
// An announce with numwant=0 doubles as a scrape: the tracker still reports
// the swarm counts (BEP 3 complete/incomplete, BEP 15 seeders/leechers).
AnnounceResult announceOne(
    const std::string& url, const std::string& hashHex, int timeoutMs, const std::function<bool()>& cancelled)
{
    AnnounceResult result;

    auto infoHash = bt::info_hash_from_hex(hashHex);
    if (!infoHash) {
        result.error = "Invalid hash";
        return result;
    }

    bt::TrackerRequest req;
    req.info_hash = *infoHash;
    req.peer_id = bt::generate_peer_id();
    req.port = kAnnouncePort;
    req.event = bt::TrackerEvent::None;
    req.left = 0;
    req.numwant = 0; // counts only, no peer list

    bt::TrackerResponse resp = bt::announce_to_tracker(url, req, timeoutMs, cancelled);
    if (resp.success) {
        result.success = true;
        result.seeders = static_cast<int>(resp.complete);
        result.leechers = static_cast<int>(resp.incomplete);
        result.completed = 0; // not reported by an announce
    } else {
        result.error = resp.failure_reason;
    }
    return result;
}

} // namespace

SwarmScraper::SwarmScraper(platform::EngineLoop& engineLoop)
    : engineLoop_(engineLoop)
    , pool_(std::make_unique<platform::WorkerPool>(kMaxPoolThreads))
{
}

SwarmScraper::~SwarmScraper()
{
    stop();
}

void SwarmScraper::stop()
{
    stopping_.store(true, std::memory_order_relaxed);
    pendingQueue_.clear();
    // Drop tasks that have not started yet, then wait for the running
    // announces to finish. In-flight tasks check stopping_ before each
    // announce, so they bail after at most the announce already in progress
    // rather than walking every remaining tracker. Waiting here guarantees no
    // pool task outlives `this`.
    if (pool_)
        pool_->stopAndDrain();
}

void SwarmScraper::requestScrape(const std::string& infoHash, const std::vector<std::string>& trackers)
{
    if (stopping_.load(std::memory_order_relaxed))
        return; // shutting down -- accept no new work

    if (infoHash.size() != static_cast<size_t>(kInfoHashHexLength)) {
        platform::log() << "SwarmScraper: ignoring invalid info-hash " << infoHash << "\n";
        return;
    }

    const std::vector<std::string>& effectiveTrackers = trackers.empty() ? kDefaultTrackers : trackers;
    if (effectiveTrackers.empty())
        return;

    // Deduplicate against the per-hash cooldown, and opportunistically prune
    // stale entries so recentChecks_ stays bounded regardless of uptime.
    const auto now = std::chrono::steady_clock::now();
    if (!havePruned_ || std::chrono::duration_cast<std::chrono::seconds>(now - lastPrune_).count() >= kCheckIntervalSecs) {
        pruneStaleChecks(now);
        lastPrune_ = now;
        havePruned_ = true;
    }

    if (const auto it = recentChecks_.find(infoHash); it != recentChecks_.end()) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
        if (elapsed < kCheckIntervalSecs) {
            platform::log() << "SwarmScraper: skipping " << infoHash.substr(0, 8) << " - checked " << elapsed
                             << " secs ago\n";
            return;
        }
    }
    recentChecks_[infoHash] = now; // mark as being checked

    // Start immediately if under the concurrency cap, otherwise queue.
    if (activeRequests_ >= kMaxConcurrent) {
        pendingQueue_.push_back({ infoHash, effectiveTrackers });
        platform::log() << "SwarmScraper: queued " << infoHash.substr(0, 8) << " - active:" << activeRequests_
                         << " queued:" << pendingQueue_.size() << "\n";
        return;
    }
    ++activeRequests_;

    startScrape(infoHash, effectiveTrackers);
}

void SwarmScraper::pruneStaleChecks(std::chrono::steady_clock::time_point now)
{
    for (auto it = recentChecks_.begin(); it != recentChecks_.end();) {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count() >= kCheckIntervalSecs)
            it = recentChecks_.erase(it);
        else
            ++it;
    }
}

void SwarmScraper::processQueue()
{
    if (stopping_.load(std::memory_order_relaxed))
        return;

    while (!pendingQueue_.empty() && activeRequests_ < kMaxConcurrent) {
        PendingRequest req = std::move(pendingQueue_.front());
        pendingQueue_.pop_front();
        ++activeRequests_;
        startScrape(req.infoHash, req.trackers);
    }
}

void SwarmScraper::startScrape(const std::string& infoHash, const std::vector<std::string>& trackers)
{
    const int timeout = kTimeoutMs;

    // A hash's trackers announce in parallel (each its own pool task) rather
    // than walking them one-by-one: different trackers see different swarm
    // slices, and parallelising caps a hash's latency at ~one announce round
    // instead of trackers * timeout. They merge into a shared best-result;
    // the last one to finish reports back on the engine thread and frees the
    // hash's slot.
    auto state = std::make_shared<ScrapeState>();
    state->remaining.store(static_cast<int>(trackers.size()));

    for (const std::string& trackerUrl : trackers) {
        pool_->post([this, infoHash, state, trackerUrl, timeout]() {
            // Skip the (blocking) announce entirely once shutdown starts, so
            // stop() drains the pool promptly instead of firing every
            // tracker. The cancel predicate also aborts an announce already
            // in progress within ~one poll slice, so stopAndDrain() returns
            // fast.
            if (!stopping_.load(std::memory_order_relaxed)) {
                AnnounceResult one = announceOne(
                    trackerUrl, infoHash, timeout, [this] { return stopping_.load(std::memory_order_relaxed); });
                if (one.success) {
                    std::lock_guard<std::mutex> locker(state->mutex);
                    if (!state->best.success || one.seeders > state->best.seeders)
                        state->best = one;
                }
            }

            // Last tracker for this hash done -> aggregate and report back.
            if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) != 1)
                return;

            AnnounceResult best;
            {
                std::lock_guard<std::mutex> locker(state->mutex);
                best = state->best;
            }
            engineLoop_.post([this, infoHash, best]() {
                if (activeRequests_ > 0)
                    --activeRequests_;

                if (best.success && scraped_)
                    scraped_(infoHash, best.seeders, best.leechers, best.completed);

                // Give any queued requests a chance to start now a slot freed.
                processQueue();
            });
        });
    }
}

} // namespace ratsn::engine
