#include "engine/indexer.h"

#include "domain/content_classifier.h"
#include "platform/engine_loop.h"
#include "platform/log.h"

#include <algorithm>

namespace ratsn::engine {

Indexer::Indexer(
    index::SearchIndex& index, domain::FilterSettings filterSettings, platform::EngineLoop& engineLoop, int indexMaxTorrents)
    : index_(index)
    , filter_(std::move(filterSettings))
    , engineLoop_(engineLoop)
    , indexMaxTorrents_(indexMaxTorrents)
    , recordCount_(index.counts().torrents)
{
}

bool Indexer::handleDiscovered(domain::Torrent torrent)
{
    domain::ContentClassifier::classify(torrent);

    if (const std::string reason = filter_.rejectionReason(torrent); !reason.empty()) {
        platform::log() << "Indexer: rejected " << torrent.hash << " \"" << torrent.name << "\": " << reason << "\n";
        return false;
    }

    const bool existedBefore = isKnownHash(torrent.hash);
    if (!index_.upsert(torrent)) {
        platform::log() << "Indexer: failed to index " << torrent.hash << "\n";
        return false;
    }
    if (existedBefore)
        return false;

    ++recordCount_;
    maybePrune();
    return true;
}

void Indexer::maybePrune()
{
    if (indexMaxTorrents_ <= 0 || pruneInFlight_)
        return;

    // Slack keeps pruning to occasional batches instead of firing on every
    // single insert once near the cap (docs/M5-PLAN.md item 8).
    const int64_t slack = std::max<int64_t>(indexMaxTorrents_ / 50, 100);
    if (recordCount_ <= indexMaxTorrents_ + slack)
        return;

    pruneInFlight_ = true;
    engineLoop_.post([this] { pruneBatch(); });
}

void Indexer::pruneBatch()
{
    constexpr int kBatchCap = 500;
    const int64_t overBy = recordCount_ - indexMaxTorrents_;
    const int limit = static_cast<int>(std::min<int64_t>(std::max<int64_t>(overBy, 0), kBatchCap));
    if (limit <= 0) {
        pruneInFlight_ = false;
        return;
    }

    const std::vector<std::string> hashes = index_.lowestValueHashes(limit);
    int removed = 0;
    for (const std::string& hash : hashes) {
        if (index_.remove(hash))
            ++removed;
    }
    recordCount_ -= removed;
    platform::log() << "Indexer: pruned " << removed << " (cap " << indexMaxTorrents_ << ")\n";

    // Re-posted (not looped in place) so other queued engine-thread tasks --
    // search, incoming wire messages -- get a turn between batches even
    // during a big overshoot. `removed > 0` guards against spinning forever
    // if lowestValueHashes ever returns fewer rows than expected.
    if (recordCount_ > indexMaxTorrents_ && removed > 0)
        engineLoop_.post([this] { pruneBatch(); });
    else
        pruneInFlight_ = false;
}

bool Indexer::isKnownHash(const std::string& hashHex)
{
    index::SearchQuery q;
    q.text = hashHex;
    q.limit = 1;
    return !index_.searchNames(q).empty();
}

} // namespace ratsn::engine
