#pragma once

#include "librats/util/json.h"

#include <cstdint>
#include <functional>
#include <string>

namespace ratsn::index {
class SearchIndex;
}

namespace ratsn::engine {

class SwarmScraper;
class TrackerSiteScraper;

// Orchestrates the two tracker scrapers and persists their results. Port of
// src/services/tracker_service.{h,cpp} (docs/M8-PLAN.md item 3). Listens for
// newly indexed torrents (via Indexer::IndexedCallback) and, when enabled,
// kicks off a seeders/leechers scrape and a website-metadata scrape; results
// flow back through the scrapers' callbacks and are written to the index.
// The scrapers themselves never touch the index -- this is the only glue
// that does. EngineLoop-confined like everything else here: both scrapers'
// callbacks already run on the engine thread.
class TrackerService {
public:
    TrackerService(SwarmScraper& swarmScraper, TrackerSiteScraper& siteScraper, index::SearchIndex& index);

    void setCountScrapingEnabled(bool enabled) { countEnabled_ = enabled; }
    void setInfoScrapingEnabled(bool enabled) { infoEnabled_ = enabled; }

    // Stop forwarding scrapes and tear down the underlying scrapers. Called
    // on shutdown so no fresh tracker work is issued while the app is
    // closing -- flags off first, then drain both scrapers (Qt's ordering,
    // kept). Call before NodeHost::stop().
    void stop();

    void checkCounts(const std::string& hash);
    void checkInfo(const std::string& hash, const std::string& name);

    // Wire to Indexer::setIndexedCallback.
    void onTorrentIndexed(const std::string& hash, const std::string& name);

    // Fired after a successful index write, on the engine thread, so the TUI
    // can refresh whichever row is currently displaying this hash --
    // docs/M8-PLAN.md deviation #4, in place of Qt's torrentUpdated signal +
    // full panel reload. trackersCheckedMs is this call's own timestamp, not
    // read back from the index -- it lands within the same synchronous call
    // as the index write, so the two can't meaningfully drift.
    using StatsCallback
        = std::function<void(const std::string& hash, int seeders, int leechers, int completed, int64_t trackersCheckedMs)>;
    using InfoCallback = std::function<void(const std::string& hash, const librats::Json& info)>;
    void setStatsUpdatedCallback(StatsCallback callback) { onStatsUpdated_ = std::move(callback); }
    void setInfoUpdatedCallback(InfoCallback callback) { onInfoUpdated_ = std::move(callback); }

private:
    void onCountsScraped(const std::string& hash, int seeders, int leechers, int completed);
    void onInfoScraped(const std::string& hash, const librats::Json& info);

    SwarmScraper& swarmScraper_;
    TrackerSiteScraper& siteScraper_;
    index::SearchIndex& index_;
    bool countEnabled_ = false;
    bool infoEnabled_ = false;
    StatsCallback onStatsUpdated_;
    InfoCallback onInfoUpdated_;
};

} // namespace ratsn::engine
