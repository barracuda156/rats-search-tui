#pragma once

#include "domain/filter_policy.h"
#include "domain/torrent.h"
#include "index/search_index.h"

#include <cstdint>
#include <functional>
#include <string>

namespace ratsn::platform {
class EngineLoop;
}

namespace ratsn::engine {

// Wires a freshly-discovered torrent through classify -> filter -> insert.
// Not a port of Qt-app code -- the Qt app wires the equivalent inline
// elsewhere; native gets its own small glue class per
// docs/DESIGN-native.md §4's module breakdown.
class Indexer {
public:
    // index and engineLoop are borrowed (non-owning) and must outlive the
    // Indexer. indexMaxTorrents <= 0 disables pruning (today's unbounded
    // behavior) -- native extension, no Qt equivalent (docs/M5-PLAN.md item
    // 8: the Indexer enforces this on the engine thread so spider +
    // replication can run indefinitely at a bounded index size).
    Indexer(index::SearchIndex& index, domain::FilterSettings filterSettings, platform::EngineLoop& engineLoop,
        int indexMaxTorrents = 0);

    // Classify, filter, and upsert if accepted. Logs the rejection reason (or
    // the successful insert) to stdout -- the explicit acceptance signal for
    // filtering per docs/DESIGN-native.md §12 ("filters demonstrably drop
    // (log line) rejected torrents"). Returns true only when the torrent was
    // genuinely new and got inserted -- false for a filtered-out, duplicate
    // (hash already indexed), or unindexable torrent. PeerApi (M4) uses this
    // to decide whether an inbound torrent counts toward replication
    // accounting.
    bool handleDiscovered(domain::Torrent torrent);

    // Crawler::KnownHashFilter-compatible: true if `hashHex` is already indexed.
    bool isKnownHash(const std::string& hashHex);

    // Fired on every successful insert in handleDiscovered (docs/M8-PLAN.md
    // item 5) -- the single write path both the crawler and PeerApi funnel
    // through, so this one hook matches Qt's torrentIndexed signal coverage
    // exactly. Set before the pipeline starts producing discoveries.
    using IndexedCallback = std::function<void(const std::string& hash, const std::string& name)>;
    void setIndexedCallback(IndexedCallback callback) { onIndexed_ = std::move(callback); }

private:
    // Checked after every genuinely-new insert; posts pruneBatch() to the
    // engine loop once recordCount_ exceeds indexMaxTorrents_ by more than
    // slack (docs/M5-PLAN.md item 8).
    void maybePrune();
    // Removes up to 500 of the lowest-value hashes (SearchIndex::
    // lowestValueHashes), then re-posts itself if still over the cap --
    // batched so a big overshoot never stalls search on one huge sweep.
    void pruneBatch();

    index::SearchIndex& index_; // borrowed
    domain::FilterPolicy filter_;
    platform::EngineLoop& engineLoop_; // borrowed
    int indexMaxTorrents_;
    int64_t recordCount_;
    bool pruneInFlight_ = false;
    IndexedCallback onIndexed_;
};

} // namespace ratsn::engine
