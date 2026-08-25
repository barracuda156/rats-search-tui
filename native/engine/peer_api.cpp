#include "engine/peer_api.h"

#include "domain/torrent_codec.h"
#include "engine/indexer.h"
#include "engine/replication.h"
#include "platform/engine_loop.h"

#include "librats/peer/peer_id.h"
#include "librats/subsystems/message_json.h"

#include <algorithm>
#include <fstream>
#include <iostream>

namespace ratsn::engine {

namespace {

// Build a SearchQuery from a peer message, tolerating both the legacy
// "navigation" wrapper and the flat top-level form newer peers send. Port of
// peer_api.cpp's anonymous parseSearchRequest.
index::SearchQuery parseSearchRequest(const librats::Json& data)
{
    index::SearchQuery q;
    q.text = data.value("text", "");
    if (q.text.empty())
        q.text = data.value("query", "");

    if (data.contains("navigation")) {
        const librats::Json& nav = *data.as_object().find("navigation");
        q.limit = nav.value("limit", 10);
        q.offset = nav.value("index", 0);
        q.sort = nav.value("orderBy", "");
        q.descending = nav.value("orderDesc", true);
        q.safeSearch = nav.value("safeSearch", false);
        q.contentType = nav.value("type", "");
    } else {
        q.limit = data.value("limit", 10);
        q.offset = data.value("index", 0);
        q.sort = data.value("orderBy", "");
        q.descending = data.value("orderDesc", true);
        q.safeSearch = data.value("safeSearch", false);
        q.contentType = data.value("type", "");
    }
    return q;
}

std::string shortId(const std::string& peerId)
{
    return peerId.substr(0, 8);
}

} // namespace

PeerApi::PeerApi(librats::MessageJson& messages, index::SearchIndex& index, Indexer& indexer,
    platform::EngineLoop& engineLoop, Replication* replication, bool replicationServerEnabled)
    : messages_(messages)
    , index_(index)
    , indexer_(indexer)
    , engineLoop_(engineLoop)
    , replication_(replication)
    , replicationServerEnabled_(replicationServerEnabled)
{
    // --- Requests we answer -------------------------------------------------
    on("searchTorrent", &PeerApi::handleSearchRequest);
    on("searchFiles", &PeerApi::handleSearchFilesRequest);
    on("topTorrents", &PeerApi::handleTopTorrentsRequest);
    on("torrent", &PeerApi::handleTorrentRequest);
    on("feed", &PeerApi::handleFeedRequest);
    on("randomTorrents", &PeerApi::handleRandomTorrentsRequest);

    // --- Responses we consume ------------------------------------------------
    on("searchTorrent_response", &PeerApi::handleSearchResult);
    on("searchFiles_response", &PeerApi::handleSearchFilesResult);
    on("torrent_response", &PeerApi::handleTorrentResponse);
    on("feed_response", &PeerApi::handleFeedResponse);
    on("randomTorrents_response", &PeerApi::handleRandomTorrentsResponse);
    on("torrentAnnounce", &PeerApi::handleTorrentAnnounce);

    std::cout << "PeerApi: handlers installed\n";
}

void PeerApi::on(const std::string& type, void (PeerApi::*handler)(const std::string&, const librats::Json&))
{
    messages_.on(type, [this, type, handler](const librats::PeerId& from, const librats::Json& data) {
        dumpWire(type, data);
        std::string peerIdHex = from.to_hex();
        engineLoop_.post([this, handler, peerIdHex, data] { (this->*handler)(peerIdHex, data); });
    });
}

void PeerApi::enableWireDump(std::filesystem::path dataDir)
{
    const std::filesystem::path dir = dataDir / "wire";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::lock_guard<std::mutex> lock(wireDumpMutex_);
    wireDumpDir_ = dir;
    std::cout << "PeerApi: wire dump enabled -> " << dir.string() << "\n";
}

void PeerApi::dumpWire(const std::string& type, const librats::Json& data)
{
    std::lock_guard<std::mutex> lock(wireDumpMutex_);
    if (!wireDumpDir_)
        return;

    const int n = ++wireDumpCounters_[type];
    const std::filesystem::path path = *wireDumpDir_ / (type + "-" + std::to_string(n) + ".json");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out)
        out << data.dump(2);
}

void PeerApi::sendToPeer(const std::string& peerIdHex, const std::string& type, const librats::Json& data)
{
    const auto id = librats::PeerId::from_hex(peerIdHex);
    if (!id) {
        std::cerr << "PeerApi: invalid peer id " << shortId(peerIdHex) << "\n";
        return;
    }
    messages_.send(*id, type, data);
}

// ============================================================================
// Requests we answer
// ============================================================================

void PeerApi::handleSearchRequest(const std::string& peerIdHex, const librats::Json& data)
{
    index::SearchQuery q = parseSearchRequest(data);
    if (q.text.length() <= 2) {
        std::cout << "PeerApi: search query too short from " << shortId(peerIdHex) << " - ignoring\n";
        return;
    }

    const std::vector<domain::SearchHit> hits = index_.searchNames(q);
    std::cout << "PeerApi: search \"" << q.text << "\" -> " << hits.size() << " results for " << shortId(peerIdHex)
               << "\n";

    for (const domain::SearchHit& hit : hits)
        sendToPeer(peerIdHex, "searchTorrent_response", domain::codec::toJson(hit.torrent));
}

void PeerApi::handleSearchFilesRequest(const std::string& peerIdHex, const librats::Json& data)
{
    index::SearchQuery q = parseSearchRequest(data);
    if (q.text.length() <= 2)
        return;

    const std::vector<domain::SearchHit> hits = index_.searchFiles(q);
    std::cout << "PeerApi: searchFiles \"" << q.text << "\" -> " << hits.size() << " results for "
               << shortId(peerIdHex) << "\n";

    for (const domain::SearchHit& hit : hits) {
        librats::Json result = domain::codec::toJson(hit);
        // Legacy peers read matching file paths from the "path" key.
        if (!hit.matchingPaths.empty()) {
            librats::Json paths = librats::Json::array();
            for (const std::string& p : hit.matchingPaths)
                paths.push_back(p);
            result["path"] = std::move(paths);
        }
        sendToPeer(peerIdHex, "searchFiles_response", result);
    }
}

void PeerApi::handleTopTorrentsRequest(const std::string& peerIdHex, const librats::Json& data)
{
    std::string type = data.value("type", "");
    std::string time;
    int offset = 0;
    int limit = 20;
    if (data.contains("navigation")) {
        const librats::Json& nav = *data.as_object().find("navigation");
        time = nav.value("time", "");
        offset = nav.value("index", 0);
        limit = nav.value("limit", 20);
    } else {
        time = data.value("time", "");
        offset = data.value("index", 0);
        limit = data.value("limit", 20);
    }

    index::TopQuery q;
    q.contentType = type;
    q.time = time;
    q.offset = offset;
    q.limit = limit;

    const std::vector<domain::Torrent> results = index_.top(q);
    std::cout << "PeerApi: topTorrents -> " << results.size() << " results for " << shortId(peerIdHex) << "\n";

    librats::Json torrents = librats::Json::array();
    for (const domain::Torrent& t : results)
        torrents.push_back(domain::codec::toJson(t));

    librats::Json response = librats::Json::object();
    response["torrents"] = std::move(torrents);
    response["type"] = type;
    response["time"] = time;
    sendToPeer(peerIdHex, "topTorrents_response", response);
}

void PeerApi::handleTorrentRequest(const std::string& peerIdHex, const librats::Json& data)
{
    const std::string hash = data.value("hash", "");
    if (hash.length() != 40)
        return;

    bool includeFiles = false;
    if (data.contains("options"))
        includeFiles = (*data.as_object().find("options")).value("files", false);
    else
        includeFiles = data.value("files", false);

    // SearchIndex has no dedicated "get by hash" -- an exact-hash query
    // through searchNames is the documented equivalent (search_index.h: "A
    // 40-hex-char query.text is treated as an exact hash lookup").
    index::SearchQuery q;
    q.text = hash;
    q.limit = 1;
    const std::vector<domain::SearchHit> hits = index_.searchNames(q);
    if (hits.empty())
        return; // not found: the wire protocol has no "miss" reply, stay silent

    std::cout << "PeerApi: torrent " << hash.substr(0, 8) << " for " << shortId(peerIdHex) << "\n";
    domain::codec::ToJsonOptions options;
    options.includeFiles = includeFiles;
    options.includeInfo = true;
    sendToPeer(peerIdHex, "torrent_response", domain::codec::toJson(hits.front().torrent, options));
}

void PeerApi::handleFeedRequest(const std::string& peerIdHex, const librats::Json& /*data*/)
{
    // TODO(M6): no FeedService yet -- answer with an empty feed so a peer
    // that asks doesn't wait on a reply that will never come.
    std::cout << "PeerApi: feed request from " << shortId(peerIdHex) << " (empty, TODO(M6))\n";

    librats::Json response = librats::Json::object();
    response["feed"] = librats::Json::array();
    response["feedDate"] = int64_t { 0 };
    response["size"] = 0;
    sendToPeer(peerIdHex, "feed_response", response);
}

void PeerApi::handleRandomTorrentsRequest(const std::string& peerIdHex, const librats::Json& data)
{
    if (!replicationServerEnabled_) {
        std::cout << "PeerApi: replication server disabled; ignoring randomTorrents from " << shortId(peerIdHex)
                   << "\n";
        return;
    }

    const int limit = std::clamp(data.value("limit", 5), 1, 10);
    const std::vector<domain::Torrent> torrents = index_.random(limit);
    std::cout << "PeerApi: randomTorrents -> " << torrents.size() << " for " << shortId(peerIdHex) << "\n";

    librats::Json array = librats::Json::array();
    const domain::codec::ToJsonOptions options { /*includeFiles*/ true, /*includeInfo*/ true };
    for (const domain::Torrent& t : torrents)
        array.push_back(domain::codec::toJson(t, options));

    librats::Json response = librats::Json::object();
    response["torrents"] = std::move(array);
    sendToPeer(peerIdHex, "randomTorrents_response", response);
}

// ============================================================================
// Responses we consume
// ============================================================================

void PeerApi::handleSearchResult(const std::string& peerIdHex, const librats::Json& data)
{
    std::string hash = data.value("hash", "");
    if (hash.empty())
        hash = data.value("info_hash", "");
    if (hash.empty())
        return;

    // Surface the hit with remote provenance stamped on. Deliberately NOT
    // indexed here: the reply carries metadata only (no file list), so
    // storing it would leave a file-less torrent in the index.
    domain::SearchHit hit = domain::codec::searchHitFromJson(data);
    hit.remote = true;
    hit.sourcePeerId = peerIdHex;
    if (onSearchResult_)
        onSearchResult_(hit);
}

void PeerApi::handleSearchFilesResult(const std::string& peerIdHex, const librats::Json& data)
{
    domain::SearchHit hit = domain::codec::searchHitFromJson(data);
    hit.fromFileMatch = true;
    hit.remote = true;
    hit.sourcePeerId = peerIdHex;
    // Legacy peers send matching paths under "path"; searchHitFromJson only
    // reads "matchingPaths" -- normalise here to match Qt's handler.
    if (hit.matchingPaths.empty() && data.contains("path")) {
        const librats::Json& paths = *data.as_object().find("path");
        if (paths.is_array()) {
            for (const librats::Json& p : paths)
                hit.matchingPaths.push_back(p.get<std::string>());
        }
    }

    const std::string query = data.value("text", "");
    std::cout << "PeerApi: file search result from " << shortId(peerIdHex) << " for " << query << "\n";
    if (onFileSearchResult_)
        onFileSearchResult_(hit);
}

void PeerApi::handleTorrentResponse(const std::string& peerIdHex, const librats::Json& data)
{
    std::string hash = data.value("hash", "");
    if (hash.empty())
        hash = data.value("info_hash", "");
    if (hash.length() != 40) {
        std::cerr << "PeerApi: invalid torrent_response from " << shortId(peerIdHex) << "\n";
        return;
    }

    insertFromPeer(data, /*trackReplication*/ true);
}

void PeerApi::handleFeedResponse(const std::string& /*peerIdHex*/, const librats::Json& /*data*/)
{
    // TODO(M6): no FeedService yet to replace/merge into.
}

void PeerApi::handleRandomTorrentsResponse(const std::string& peerIdHex, const librats::Json& data)
{
    if (!data.contains("torrents"))
        return;
    const librats::Json& torrents = *data.as_object().find("torrents");
    if (!torrents.is_array())
        return;

    int inserted = 0;
    for (const librats::Json& v : torrents) {
        if (v.is_object() && insertFromPeer(v, /*trackReplication*/ true))
            ++inserted;
    }
    if (inserted > 0)
        std::cout << "PeerApi: replicated " << inserted << " torrents from " << shortId(peerIdHex) << "\n";
}

void PeerApi::handleTorrentAnnounce(const std::string& peerIdHex, const librats::Json& data)
{
    const std::string hash = data.value("info_hash", "");
    const std::string name = data.value("name", "");
    if (hash.empty() || name.empty())
        return;

    std::cout << "PeerApi: torrent announce from " << shortId(peerIdHex) << ": " << name << "\n";
    insertFromPeer(data, /*trackReplication*/ false);
}

// ============================================================================
// Broadcasts / peer lifecycle
// ============================================================================

void PeerApi::broadcastSearch(const std::string& query, int limit, const std::string& sort, bool descending,
    bool safeSearch, const std::string& contentType, bool searchFiles)
{
    if (query.length() <= 2)
        return;

    librats::Json msg = librats::Json::object();
    msg["query"] = query;
    msg["text"] = query;
    msg["limit"] = limit;
    msg["orderBy"] = sort;
    msg["orderDesc"] = descending;
    msg["safeSearch"] = safeSearch;
    if (!contentType.empty())
        msg["type"] = contentType;

    messages_.send("searchTorrent", msg);
    if (searchFiles)
        messages_.send("searchFiles", msg);
}

void PeerApi::onPeerConnected(const std::string& peerIdHex)
{
    if (replication_ && replication_->isEnabled()) {
        librats::Json data = librats::Json::object();
        data["limit"] = 5;
        sendToPeer(peerIdHex, "randomTorrents", data);
    }
}

// ============================================================================
// Insertion (single write path)
// ============================================================================

bool PeerApi::insertFromPeer(const librats::Json& data, bool trackReplication)
{
    domain::Torrent torrent = domain::codec::torrentFromJson(data);
    if (torrent.hash.length() != 40)
        return false;

    const bool inserted = indexer_.handleDiscovered(std::move(torrent));
    if (!inserted)
        return false;

    if (trackReplication && replication_)
        replication_->notifyReceived();

    return true;
}

} // namespace ratsn::engine
