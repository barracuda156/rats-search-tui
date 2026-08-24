#pragma once

#include "domain/filter_policy.h"
#include "domain/torrent.h"
#include "index/search_index.h"

#include <string>

namespace ratsn::engine {

// Wires a freshly-discovered torrent through classify -> filter -> insert.
// Not a port of Qt-app code -- the Qt app wires the equivalent inline
// elsewhere; native gets its own small glue class per
// docs/DESIGN-native.md §4's module breakdown.
class Indexer {
public:
    // index is borrowed (non-owning) and must outlive the Indexer.
    Indexer(index::SearchIndex& index, domain::FilterSettings filterSettings);

    // Classify, filter, and upsert if accepted. Logs the rejection reason (or
    // the successful insert) to stdout -- the explicit acceptance signal for
    // filtering per docs/DESIGN-native.md §12 ("filters demonstrably drop
    // (log line) rejected torrents").
    void handleDiscovered(domain::Torrent torrent);

    // Crawler::KnownHashFilter-compatible: true if `hashHex` is already indexed.
    bool isKnownHash(const std::string& hashHex);

private:
    index::SearchIndex& index_; // borrowed
    domain::FilterPolicy filter_;
};

} // namespace ratsn::engine
