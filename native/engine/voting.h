#pragma once

#include "domain/torrent.h"
#include "index/search_index.h"
#include "librats/util/json.h"

#include <functional>
#include <string>
#include <unordered_set>

namespace ratsn::engine {

class P2PStore;
struct StoredRecord;

// Aggregated result of counting all vote records for a hash.
struct VoteCounts {
    int good = 0;
    int bad = 0;
    bool selfVoted = false;
};

// Native port of src/services/voting_service.{h,cpp} (docs/M7-PLAN.md item
// 3): torrent up/down voting over the distributed store. Each vote is a
// record keyed "vote:{hash}:{peerId}" (one per peer per torrent) so
// aggregation across all peers' records yields swarm-wide good/bad counts;
// the record format and key layout are preserved verbatim so already-
// replicated Qt-peer votes keep aggregating. Callback-based instead of Qt
// signals; EngineLoop-confined like every other engine component -- callers
// must already be on that thread. The aggregated counts are also mirrored
// onto the torrent's good/bad index columns for fast local reads.
class Voting {
public:
    // Result callback, mirroring the API response shape: (ok, data, error).
    using ResultCallback = std::function<void(bool ok, const librats::Json& result, const std::string& error)>;
    // Native stand-in for Qt's votesUpdated signal; main.cpp wires this to
    // FeedService::addByHash (docs/M7-PLAN.md item 6).
    using VotesUpdatedCallback = std::function<void(const std::string& hash, int good, int bad)>;

    // store/index are borrowed and must outlive this object.
    Voting(P2PStore& store, index::SearchIndex& index);

    // Cast a vote for `hash`. Idempotent per peer: a second call returns the
    // current counts with alreadyVoted=true instead of double counting.
    void vote(const std::string& hash, bool isGood, ResultCallback callback = {});

    // Fetch aggregated votes for `hash`. Uses the distributed store when
    // available, otherwise falls back to the local torrent's good/bad columns.
    void getVotes(const std::string& hash, ResultCallback callback);

    // Whether we have already stored a vote for `hash`.
    bool hasVoted(const std::string& hash) const;

    void setVotesUpdatedCallback(VotesUpdatedCallback callback) { onVotesUpdated_ = std::move(callback); }

private:
    // Count all vote records for `hash` across peers (from the distributed store).
    VoteCounts aggregate(const std::string& hash) const;

    bool storeVote(const std::string& hash, bool isGood, const librats::Json& torrentData);
    void onRecordStored(const StoredRecord& record, bool isRemote);

    P2PStore& store_;
    index::SearchIndex& index_;

    // Hashes this instance has already voted on, so repeated votes are
    // deduped even when the distributed store is unavailable (hasVoted() can
    // only answer when the store is up). Persistent dedup still comes from
    // the store records.
    std::unordered_set<std::string> selfVotedHashes_;
    VotesUpdatedCallback onVotesUpdated_;
};

} // namespace ratsn::engine
