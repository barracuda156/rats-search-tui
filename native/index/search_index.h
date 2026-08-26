#pragma once

#include "domain/torrent.h"
#include "librats/util/json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// A narrow backend interface (docs/DESIGN-native.md §5) so an SQLite-FTS5
// backend can be swapped in later without touching engine code. GroongaIndex
// is the only implementation for now.
namespace ratsn::index {

struct SearchQuery {
    std::string text;
    int offset = 0;
    int limit = 10;
    std::string sort; // "seeders","leechers","name","size","files","added","completed"; "" = relevance
    bool descending = true;
    bool safeSearch = false;
    std::string contentType; // "", "video".."archive", or "application" (Software+Games)
    int64_t sizeMin = 0;
    int64_t sizeMax = 0;
    int filesMin = 0;
    int filesMax = 0;
    int seedersMin = 0;
    std::string tracker; // "" = any; otherwise a tracker name from info["trackers"]
    // Disables Groonga's match_escalation (see docs/M5-PLAN.md "Why strict
    // matching"): an unrelated result never outranks/replaces "no results".
    // Default true -- matches the M5 project-owner decision to make strict
    // the default, with a UI/CLI toggle back to loose.
    bool strict = true;
};

struct TopQuery {
    std::string contentType; // "" = any
    std::string time; // "", "hours", "week", "month"
    int offset = 0;
    int limit = 10;
};

struct IndexStats {
    int64_t torrents = 0;
    int64_t files = 0;
    int64_t totalSize = 0;
};

class SearchIndex {
public:
    virtual ~SearchIndex() = default;

    // Insert, or update every column if `torrent.hash` already exists.
    virtual bool upsert(const domain::Torrent& torrent) = 0;
    virtual bool remove(const std::string& hash) = 0;

    // A 40-hex-char `query.text` is treated as an exact hash lookup rather
    // than a full-text query, mirroring src/data/torrent_repository.cpp.
    virtual std::vector<domain::SearchHit> searchNames(const SearchQuery& query) = 0;
    virtual std::vector<domain::SearchHit> searchFiles(const SearchQuery& query) = 0;
    virtual std::vector<domain::Torrent> top(const TopQuery& query) = 0;
    virtual std::vector<domain::Torrent> random(int limit) = 0;

    // Partial update of the tracker-scrape columns only; also stamps
    // trackers_checked = now (docs/M8-PLAN.md item 4).
    virtual bool updateStats(const std::string& hash, int seeders, int leechers, int completed) = 0;
    // Shallow-merges `info`'s keys onto the row's existing `info` object
    // (docs/M8-PLAN.md item 4 -- port of data::TorrentRepository::mergeInfo).
    virtual bool mergeInfo(const std::string& hash, const librats::Json& info) = 0;
    // Partial update of the good/bad vote columns only (docs/M7-PLAN.md item
    // 3 -- port of data::TorrentRepository::update's good/bad columns, as
    // used by VotingService's local-column mirroring).
    virtual bool updateVotes(const std::string& hash, int good, int bad) = 0;
    virtual IndexStats counts() = 0;

    // Zero-seeder-oldest-first hashes, for Indexer's indexMaxTorrents pruning
    // (docs/M5-PLAN.md item 8; native extension, no Qt equivalent).
    virtual std::vector<std::string> lowestValueHashes(int limit) = 0;
};

} // namespace ratsn::index
