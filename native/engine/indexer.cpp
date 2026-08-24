#include "engine/indexer.h"

#include "domain/content_classifier.h"

#include <iostream>

namespace ratsn::engine {

Indexer::Indexer(index::SearchIndex& index, domain::FilterSettings filterSettings)
    : index_(index)
    , filter_(std::move(filterSettings))
{
}

void Indexer::handleDiscovered(domain::Torrent torrent)
{
    domain::ContentClassifier::classify(torrent);

    if (const std::string reason = filter_.rejectionReason(torrent); !reason.empty()) {
        std::cout << "Indexer: rejected " << torrent.hash << " \"" << torrent.name << "\": " << reason << "\n";
        return;
    }

    if (!index_.upsert(torrent))
        std::cerr << "Indexer: failed to index " << torrent.hash << "\n";
}

bool Indexer::isKnownHash(const std::string& hashHex)
{
    index::SearchQuery q;
    q.text = hashHex;
    q.limit = 1;
    return !index_.searchNames(q).empty();
}

} // namespace ratsn::engine
