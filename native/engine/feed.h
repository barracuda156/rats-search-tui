#pragma once

#include "domain/torrent.h"
#include "index/search_index.h"
#include "librats/util/json.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ratsn::platform {
class EngineLoop;
}

namespace ratsn::engine {

// A single entry in the voted-torrents feed: a domain torrent plus the
// moment it entered the feed. `feedDate` is seconds since the Unix epoch
// (matching Qt's QDateTime::currentSecsSinceEpoch() -- NOT
// domain::Torrent::added's milliseconds unit) and drives the recency half of
// the ranking; everything else lives on the torrent itself (votes, seeders,
// files).
struct FeedItem {
    domain::Torrent torrent;
    int64_t feedDate = 0;
};

// Native port of src/services/feed_service.{h,cpp} (docs/M7-PLAN.md item 4):
// the ranked, in-memory voted-torrents feed. Items are ordered by a
// recency+votes score (see calculateScore) and capped at maxSize_.
// EngineLoop-confined like every other engine component -- callers must
// already be on that thread. Persistence goes to <dataDir>/feed.json
// (deviation #2: Qt stores identical item JSON as rows of a Manticore `feed`
// table instead of a flat file).
//
// Persistence is debounced: mutations only mark the feed dirty and a short
// coalescing EngineLoop::postDelayed flush rewrites the whole file. save()
// forces an immediate flush; the destructor flushes if dirty.
class Feed {
public:
    // index is borrowed and must outlive this object (addByHash looks
    // torrents up there). engineLoop is borrowed and drives the debounced
    // persistence.
    Feed(index::SearchIndex& index, platform::EngineLoop& engineLoop, std::string filePath);
    ~Feed();

    Feed(const Feed&) = delete;
    Feed& operator=(const Feed&) = delete;

    // Load the feed from filePath. Blocked content is filtered out on load.
    bool load();
    // Force an immediate persist of the current feed (bypasses the debounce).
    bool save();

    int size() const { return static_cast<int>(feed_.size()); }
    int64_t feedDate() const { return feedDate_; }

    // Look the hash up in the index and add it. This is the ONLY entry path
    // into the feed (docs/M7-PLAN.md: "the feed is a voted-torrents feed") --
    // main.cpp wires Voting's votesUpdated callback here.
    void addByHash(const std::string& hash);

    // Page of feed items, ranked.
    std::vector<FeedItem> getFeed(int index = 0, int limit = 20) const;

    // JSON representation used by the P2P feed exchange.
    librats::Json toJsonArray(int index = 0, int limit = 20) const;
    // Replace the whole feed from a remote peer's JSON (P2P sync).
    void fromJsonArray(const librats::Json& array, int64_t remoteFeedDate = 0);

    // Bumped on every mutation -- the native stand-in for Qt's feedUpdated
    // signal. Unlike DownloadManager::revision() (only ever read from its
    // own EngineLoop-thread poll), FeedTab polls this directly from the UI
    // thread's per-frame check (docs/M7-PLAN.md item 7: "the Top tab's
    // activation check already runs per-frame") to notice a vote/feed-sync
    // update while the tab is already visible -- hence atomic here.
    uint64_t revision() const { return revision_.load(std::memory_order_relaxed); }

private:
    // Add or update a torrent in the feed. Existing items keep their
    // feedDate but refresh votes/seeders; new items get the current time if
    // unset.
    void add(const FeedItem& item);

    void reorder();
    void rebuildIndex();
    double calculateScore(const FeedItem& item) const;

    // Debounced persistence.
    void markDirty();
    void flush(); // flush only if dirty (postDelayed callback)
    bool persistNow(); // always rewrite the feed file

    // Serialise the whole feed to the stored JSON shape.
    librats::Json toStoredArray() const;

    index::SearchIndex& index_;
    platform::EngineLoop& engineLoop_;
    std::string filePath_;

    std::vector<FeedItem> feed_;
    std::unordered_map<std::string, size_t> indexByHash_; // hash -> position in feed_ (kept in sync with reorder)
    int maxSize_ = 1000;
    int64_t feedDate_ = 0;
    bool dirty_ = false;
    bool flushScheduled_ = false;
    std::atomic<uint64_t> revision_ { 0 };
};

} // namespace ratsn::engine
