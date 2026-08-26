#include "engine/voting.h"

#include "domain/torrent_codec.h"
#include "engine/p2p_store.h"
#include "platform/log.h"

namespace ratsn::engine {

namespace {
bool isValidHash(const std::string& hash)
{
    return hash.length() == 40;
}
} // namespace

Voting::Voting(P2PStore& store, index::SearchIndex& index) : store_(store), index_(index)
{
    store_.setListener([this](const StoredRecord& record, bool isRemote) { onRecordStored(record, isRemote); });
    platform::log() << "Voting: initialized\n";
}

void Voting::vote(const std::string& hash, bool isGood, ResultCallback callback)
{
    platform::log() << "Voting: vote hash=" << hash.substr(0, 16) << " isGood=" << (isGood ? "true" : "false") << "\n";

    if (!isValidHash(hash)) {
        if (callback)
            callback(false, librats::Json(), "Invalid hash");
        return;
    }

    index::SearchQuery lookup;
    lookup.text = hash;
    lookup.limit = 1;
    std::vector<domain::SearchHit> hits = index_.searchNames(lookup);
    if (hits.empty()) {
        if (callback)
            callback(false, librats::Json(), "Torrent not found");
        return;
    }
    domain::Torrent torrent = std::move(hits.front().torrent);

    const bool storeReady = store_.isAvailable();

    // Already voted: return the current counts without double counting. The
    // store gives cross-session dedup when available; the in-memory set
    // covers the case where the store is down (otherwise every repeated
    // click would keep bumping the local good/bad columns without bound).
    if (selfVotedHashes_.count(hash) || (storeReady && hasVoted(hash))) {
        int goodCount = torrent.good;
        int badCount = torrent.bad;
        if (storeReady) {
            const VoteCounts votes = aggregate(hash);
            goodCount = votes.good;
            badCount = votes.bad;
        }
        platform::log() << "Voting: already voted on " << hash.substr(0, 8) << " good=" << goodCount
                         << " bad=" << badCount << "\n";

        librats::Json result = librats::Json::object();
        result["hash"] = hash;
        result["good"] = goodCount;
        result["bad"] = badCount;
        result["selfVoted"] = true;
        result["alreadyVoted"] = true;
        if (callback)
            callback(true, result, "");
        return;
    }

    // Store the vote in the distributed store (this replicates to all peers).
    bool storedInP2P = false;
    if (storeReady) {
        librats::Json torrentData = domain::codec::toJson(torrent, { /*includeFiles*/ true, /*includeInfo*/ true });
        storedInP2P = storeVote(hash, isGood, torrentData);
    }

    // Mirror the vote onto the local torrent's index columns for fast local access.
    if (isGood)
        torrent.good++;
    else
        torrent.bad++;
    index_.updateVotes(hash, torrent.good, torrent.bad);
    selfVotedHashes_.insert(hash);

    // Prefer the distributed aggregate for the returned counts.
    int goodCount = torrent.good;
    int badCount = torrent.bad;
    if (storeReady) {
        const VoteCounts votes = aggregate(hash);
        goodCount = votes.good;
        badCount = votes.bad;
    }

    if (onVotesUpdated_)
        onVotesUpdated_(hash, goodCount, badCount);

    librats::Json result = librats::Json::object();
    result["hash"] = hash;
    result["good"] = goodCount;
    result["bad"] = badCount;
    result["selfVoted"] = true;
    result["distributed"] = storedInP2P;
    if (callback)
        callback(true, result, "");
}

void Voting::getVotes(const std::string& hash, ResultCallback callback)
{
    if (!isValidHash(hash)) {
        if (callback)
            callback(false, librats::Json(), "Invalid hash");
        return;
    }

    librats::Json result = librats::Json::object();
    result["hash"] = hash;

    if (store_.isAvailable()) {
        // Aggregates every peer's vote records.
        const VoteCounts votes = aggregate(hash);
        result["good"] = votes.good;
        result["bad"] = votes.bad;
        result["selfVoted"] = votes.selfVoted;
        result["source"] = "distributed";
    } else {
        // Fall back to the local torrent's columns.
        index::SearchQuery lookup;
        lookup.text = hash;
        lookup.limit = 1;
        std::vector<domain::SearchHit> hits = index_.searchNames(lookup);
        if (!hits.empty()) {
            result["good"] = hits.front().torrent.good;
            result["bad"] = hits.front().torrent.bad;
            result["selfVoted"] = false; // cannot be determined locally
            result["source"] = "local";
        } else {
            result["good"] = 0;
            result["bad"] = 0;
            result["selfVoted"] = false;
            result["source"] = "none";
        }
    }

    if (callback)
        callback(true, result, "");
}

VoteCounts Voting::aggregate(const std::string& hash) const
{
    VoteCounts result;

    if (!isValidHash(hash) || !store_.isAvailable())
        return result;

    const std::string peerId = store_.ourPeerId();

    // One record per peer: "vote:{hash}:{peerId}".
    const std::string prefix = "vote:" + hash + ":";
    const std::vector<StoredRecord> records = store_.find(prefix);

    for (const StoredRecord& record : records) {
        const std::string vote = record.data.value("vote", "");
        if (vote == "good")
            result.good++;
        else if (vote == "bad")
            result.bad++;

        if (record.peerId == peerId)
            result.selfVoted = true;
    }

    return result;
}

bool Voting::hasVoted(const std::string& hash) const
{
    if (!isValidHash(hash) || !store_.isAvailable())
        return false;

    return store_.has("vote:" + hash + ":" + store_.ourPeerId());
}

bool Voting::storeVote(const std::string& hash, bool isGood, const librats::Json& torrentData)
{
    if (!store_.isAvailable()) {
        platform::log() << "Voting: storage not available for voting\n";
        return false;
    }

    const std::string peerId = store_.ourPeerId();

    // Key format: vote:{hash}:{peerId} -- one vote per peer per torrent.
    const std::string key = "vote:" + hash + ":" + peerId;

    librats::Json voteData = librats::Json::object();
    voteData["type"] = "vote";
    voteData["torrentHash"] = hash;
    voteData["vote"] = isGood ? "good" : "bad";
    voteData["_index"] = "vote:" + hash;
    // Included for peers that do not have this torrent yet -- written, never
    // read back on receive (see onRecordStored: aggregation only).
    voteData["_torrent"] = torrentData;

    const bool result = store_.put(key, std::move(voteData));
    if (result)
        platform::log() << "Voting: stored " << (isGood ? "good" : "bad") << " vote for " << hash.substr(0, 8) << "\n";
    return result;
}

void Voting::onRecordStored(const StoredRecord& record, bool isRemote)
{
    // Local votes already fire onVotesUpdated_ from vote(); only react to peers'.
    if (!isRemote || record.type != "vote")
        return;

    const std::string hash = record.data.value("torrentHash", "");
    if (!isValidHash(hash))
        return;

    // A peer's vote changed the swarm aggregate: mirror it onto the local
    // index columns (so offline reads stay correct) and notify the feed/UI.
    const VoteCounts votes = aggregate(hash);

    index::SearchQuery lookup;
    lookup.text = hash;
    lookup.limit = 1;
    std::vector<domain::SearchHit> hits = index_.searchNames(lookup);
    if (!hits.empty() && (hits.front().torrent.good != votes.good || hits.front().torrent.bad != votes.bad))
        index_.updateVotes(hash, votes.good, votes.bad);

    if (onVotesUpdated_)
        onVotesUpdated_(hash, votes.good, votes.bad);
}

} // namespace ratsn::engine
