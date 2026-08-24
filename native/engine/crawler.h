#pragma once

#include "domain/torrent.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_set>

namespace librats {
class Bittorrent;
}

namespace ratsn::platform {
class EngineLoop;
}

namespace ratsn::engine {

// DHT crawler: walks the DHT to expand the routing table, captures
// announce_peer messages from other clients (spider mode), fetches BEP 9
// metadata for freshly discovered info-hashes, and hands back the resulting
// domain::Torrent -- and nothing more. Never touches the search index;
// persistence/filtering/classification is the caller's job (see Indexer),
// driven off the discovered-torrent callback. Port of src/net/crawler.{h,cpp}
// (+ src/net/librats_convert.cpp for the TorrentInfo -> Torrent conversion).
class Crawler {
public:
    // bittorrent and engineLoop are borrowed (non-owning) and must outlive
    // the crawler.
    Crawler(librats::Bittorrent* bittorrent, platform::EngineLoop& engineLoop);
    ~Crawler();

    Crawler(const Crawler&) = delete;
    Crawler& operator=(const Crawler&) = delete;

    bool start();
    void stop();
    bool isRunning() const { return running_; }

    void setWalkInterval(int intervalMs) { walkIntervalMs_ = intervalMs; }

    // Consulted for each freshly seen info-hash before a BEP 9 metadata fetch
    // is queued: returning true skips the fetch (e.g. already indexed). Keeps
    // the crawler index-free -- the caller injects the lookup. Set before
    // start(); called on the EngineLoop thread (onAnnounce is already
    // marshalled there).
    using KnownHashFilter = std::function<bool(const std::string& infoHashHex)>;
    void setKnownHashFilter(KnownHashFilter filter) { knownHashFilter_ = std::move(filter); }

    // Invoked once metadata has been fetched and a full torrent model built.
    // Set before start(); called on the EngineLoop thread.
    using DiscoveredCallback = std::function<void(const domain::Torrent&)>;
    void setDiscoveredCallback(DiscoveredCallback callback) { discovered_ = std::move(callback); }

    int discoveredCount() const { return discoveredCount_.load(); }
    int activeFetches() const { return activeFetches_.load(); }

private:
    // A pending metadata fetch. An empty peerIp means "no announcing peer
    // known" -- fall back to a DHT search (slow path) instead of a direct
    // peer connection (fast path). A failed fast-path fetch is re-queued
    // once with the peer cleared (see fetchMetadata), so the slow path also
    // serves as the single retry; a slow-path failure is final.
    struct MetadataRequest {
        std::string infoHash; // lower-case hex
        std::string peerIp;
        uint16_t peerPort = 0;
    };

    // Self-reschedules via EngineLoop::postDelayed (no QTimer here); each
    // checks running_ on entry so stop() ends the chain without an explicit
    // cancel.
    void onSpiderWalk();
    void onIgnoreToggle();
    void processMetadataQueue();

    // announce_peer callback, already marshalled onto the EngineLoop thread.
    void onAnnounce(const std::array<uint8_t, 20>& infoHash, const std::string& ip, uint16_t port);

    // Kick off a BEP 9 metadata fetch (fast path if peer is known, else DHT).
    void fetchMetadata(const MetadataRequest& request);

    // Handle a completed metadata fetch (runs on the EngineLoop thread).
    void onMetadataReceived(const domain::Torrent& torrent);

    static constexpr int DEFAULT_WALK_INTERVAL_MS = 100;
    static constexpr int DEFAULT_IGNORE_INTERVAL_MS = 1000;
    static constexpr int METADATA_QUEUE_INTERVAL_MS = 100;
    static constexpr int MAX_CONCURRENT_METADATA_FETCHES = 10;
    static constexpr size_t MAX_RECENT_HASHES = 10000;

    librats::Bittorrent* bittorrent_; // borrowed
    platform::EngineLoop& engineLoop_; // borrowed

    std::atomic<bool> running_ { false };
    std::atomic<int> discoveredCount_ { 0 };
    std::atomic<int> activeFetches_ { 0 };

    int walkIntervalMs_ = DEFAULT_WALK_INTERVAL_MS;

    std::queue<MetadataRequest> metadataQueue_;
    std::mutex queueMutex_;

    // unordered_set rather than the Qt version's std::set: this is a pure
    // bounded dedup cache (eviction order was never meaningful), and lookups
    // here are the hot path on every DHT announce.
    std::unordered_set<std::string> recentHashes_;
    std::mutex recentHashesMutex_;

    KnownHashFilter knownHashFilter_;
    DiscoveredCallback discovered_;
};

} // namespace ratsn::engine
