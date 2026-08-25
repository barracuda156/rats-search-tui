#pragma once

#include "domain/torrent.h"
#include "index/search_index.h"
#include "librats/util/json.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace librats {
class MessageJson;
}

namespace ratsn::platform {
class EngineLoop;
}

namespace ratsn::engine {

class Indexer;
class Replication;

// The P2P front-end. Mirrors Qt's PeerApi (src/peer/peer_api.{h,cpp}): answers
// other peers' requests (search/files/top/torrent/feed/randomTorrents) by
// running queries straight against the local SearchIndex, and processes their
// responses by funnelling every received torrent through the one Indexer
// write path -- this never touches the index directly. Wire message-type
// names and JSON field names are copied verbatim so already-deployed peers
// keep interoperating (docs/M4-PLAN.md).
//
// MessageJson::on()/send() are documented thread-safe at any time (unlike
// librats::Node::on_peer_connected, which PeerRegistry uses and which must be
// registered before Node::start()), so unlike PeerRegistry this can be
// constructed any time after NodeHost::start() returns. Callbacks from
// librats fire on a reactor thread and are marshalled onto the EngineLoop
// before touching any state, same pattern as Crawler::onAnnounce.
class PeerApi {
public:
    // messages, index, indexer and engineLoop are borrowed (non-owning) and
    // must outlive this object. replication may be null (replication
    // disabled entirely); when non-null, notifyReceived() is called for
    // every newly inserted torrent that counts toward its cycle accounting,
    // and its isEnabled() gates the connect-time replication kick.
    // replicationServerEnabled gates whether randomTorrents requests from
    // other peers are answered at all (Qt key p2pReplicationServer).
    PeerApi(librats::MessageJson& messages, index::SearchIndex& index, Indexer& indexer,
        platform::EngineLoop& engineLoop, Replication* replication, bool replicationServerEnabled);

    // A remote peer sent a search/file-search hit. Fired on the EngineLoop
    // thread; the TUI marshals onward to the UI thread itself (see
    // tui/search_tab.cpp). remote/sourcePeerId are already stamped on.
    using SearchResultCallback = std::function<void(const domain::SearchHit&)>;
    void setSearchResultCallback(SearchResultCallback cb) { onSearchResult_ = std::move(cb); }
    void setFileSearchResultCallback(SearchResultCallback cb) { onFileSearchResult_ = std::move(cb); }

    // Broadcast a search to every connected peer (TUI remote search merge,
    // docs/M4-PLAN.md). No-op for queries of length <= 2, matching the wire's
    // own guard in handleSearchRequest below (mirrors mainwindow.cpp ~819).
    // searchFiles also broadcasts "searchFiles" alongside "searchTorrent".
    void broadcastSearch(const std::string& query, int limit, const std::string& sort, bool descending,
        bool safeSearch, const std::string& contentType, bool searchFiles);

    // Peer lifecycle follow-up: call once per newly connected peer (wired via
    // PeerRegistry::setPeerConnectedCallback, see node_host.cpp). Asks for an
    // initial replication batch when replication is enabled. The Qt original
    // also pulls the peer's feed here; omitted -- TODO(M6), no FeedService
    // yet to receive it.
    void onPeerConnected(const std::string& peerIdHex);

    // Fixture capture for the golden-file tests (docs/M4-PLAN.md): once
    // enabled, every received message's raw JSON is logged to
    // <dataDir>/wire/<type>-<n>.json. Off by default; main.cpp calls this
    // only when the RATSN_WIRE_DUMP env var is set. Safe to call any time,
    // from any thread.
    void enableWireDump(std::filesystem::path dataDir);

private:
    // Request handlers (we answer these) ---------------------------------
    void handleSearchRequest(const std::string& peerIdHex, const librats::Json& data);
    void handleSearchFilesRequest(const std::string& peerIdHex, const librats::Json& data);
    void handleTopTorrentsRequest(const std::string& peerIdHex, const librats::Json& data);
    void handleTorrentRequest(const std::string& peerIdHex, const librats::Json& data);
    void handleFeedRequest(const std::string& peerIdHex, const librats::Json& data);
    void handleRandomTorrentsRequest(const std::string& peerIdHex, const librats::Json& data);

    // Response handlers (we consume these) --------------------------------
    void handleSearchResult(const std::string& peerIdHex, const librats::Json& data);
    void handleSearchFilesResult(const std::string& peerIdHex, const librats::Json& data);
    void handleTorrentResponse(const std::string& peerIdHex, const librats::Json& data);
    void handleFeedResponse(const std::string& peerIdHex, const librats::Json& data);
    void handleRandomTorrentsResponse(const std::string& peerIdHex, const librats::Json& data);
    void handleTorrentAnnounce(const std::string& peerIdHex, const librats::Json& data);

    // Registers `handler` on `type`, marshalling onto the EngineLoop thread.
    void on(const std::string& type, void (PeerApi::*handler)(const std::string&, const librats::Json&));
    void sendToPeer(const std::string& peerIdHex, const std::string& type, const librats::Json& data);
    // Called (if enabled) on the reactor thread that delivered `data`, before
    // marshalling -- see enableWireDump.
    void dumpWire(const std::string& type, const librats::Json& data);

    // Insert one received torrent through Indexer (the single write path).
    // Returns true only when a genuinely new torrent was stored. When
    // trackReplication is set, a new insert also pings Replication.
    bool insertFromPeer(const librats::Json& data, bool trackReplication);

    librats::MessageJson& messages_;
    index::SearchIndex& index_;
    Indexer& indexer_;
    platform::EngineLoop& engineLoop_;
    Replication* replication_;
    bool replicationServerEnabled_;

    SearchResultCallback onSearchResult_;
    SearchResultCallback onFileSearchResult_;

    std::mutex wireDumpMutex_;
    std::optional<std::filesystem::path> wireDumpDir_;
    std::unordered_map<std::string, int> wireDumpCounters_; // guarded by wireDumpMutex_
};

} // namespace ratsn::engine
