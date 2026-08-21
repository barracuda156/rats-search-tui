#pragma once

#include "domain/torrent.h"

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

    // Partial update of the tracker-scrape columns only.
    virtual bool updateStats(const std::string& hash, int seeders, int leechers, int completed) = 0;
    virtual IndexStats counts() = 0;
};

} // namespace ratsn::index
