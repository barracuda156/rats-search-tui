#include "engine/crawler.h"

#include "platform/engine_loop.h"

#include "librats/bittorrent/torrent_info.h"
#include "librats/subsystems/bittorrent.h"

#include <chrono>
#include <iostream>

namespace ratsn::engine {

namespace {

std::string toHex(const std::array<uint8_t, 20>& bytes)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(40);
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0F]);
    }
    return out;
}

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Port of src/net/librats_convert.cpp's toDomainTorrent, minus the
// ipv4/port fields (peer-connection metadata; native's domain::Torrent
// doesn't carry them -- they're not part of the persisted/searchable schema).
domain::Torrent toDomainTorrent(const std::string& infoHashHex, const librats::bittorrent::TorrentInfo& info)
{
    domain::Torrent torrent;
    torrent.hash = infoHashHex;
    torrent.name = info.name();
    torrent.size = static_cast<int64_t>(info.total_size());
    torrent.files = static_cast<int>(info.files().files().size());
    torrent.pieceLength = static_cast<int>(info.piece_length());
    torrent.added = nowMs();

    for (const auto& file : info.files().files()) {
        domain::File f;
        f.path = file.path;
        f.size = static_cast<int64_t>(file.size);
        torrent.fileList.push_back(std::move(f));
    }
    return torrent;
}

} // namespace

Crawler::Crawler(librats::Bittorrent* bittorrent, platform::EngineLoop& engineLoop)
    : bittorrent_(bittorrent)
    , engineLoop_(engineLoop)
{
}

Crawler::~Crawler()
{
    stop();
}

bool Crawler::start()
{
    if (running_)
        return true;

    if (!bittorrent_) {
        std::cerr << "Crawler: BitTorrent subsystem not available\n";
        return false;
    }

    std::cout << "Starting DHT crawler...\n";

    // Enable spider mode with an announce callback. The core delivers the
    // info-hash already decoded and the peer as a structured Address, so no
    // string parsing is needed. The callback fires on a librats worker
    // thread, so marshal it onto the EngineLoop thread before touching state.
    bittorrent_->set_spider_mode(true);
    bittorrent_->set_spider_announce_callback([this](const librats::InfoHash& infoHash, const librats::Address& peer) {
        std::array<uint8_t, 20> hash = infoHash;
        std::string ip = peer.ip.to_string();
        uint16_t port = peer.port;
        engineLoop_.post([this, hash, ip, port] { onAnnounce(hash, ip, port); });
    });

    running_ = true;

    engineLoop_.postDelayed([this] { onSpiderWalk(); }, walkIntervalMs_);
    engineLoop_.postDelayed([this] { onIgnoreToggle(); }, DEFAULT_IGNORE_INTERVAL_MS);
    engineLoop_.postDelayed([this] { processMetadataQueue(); }, METADATA_QUEUE_INTERVAL_MS);

    std::cout << "DHT crawler started successfully\n";
    return true;
}

void Crawler::stop()
{
    if (!running_)
        return;

    std::cout << "Stopping DHT crawler...\n";

    if (bittorrent_)
        bittorrent_->set_spider_mode(false);
    activeFetches_ = 0;

    // Note: we never stop the BitTorrent subsystem here -- NodeHost owns it.

    running_ = false;

    std::cout << "DHT crawler stopped. Total discovered: " << discoveredCount_.load() << "\n";
}

void Crawler::onSpiderWalk()
{
    if (!running_)
        return;

    if (bittorrent_)
        bittorrent_->spider_walk();

    engineLoop_.postDelayed([this] { onSpiderWalk(); }, walkIntervalMs_);
}

void Crawler::onIgnoreToggle()
{
    if (!running_)
        return;

    if (bittorrent_ && bittorrent_->is_spider_mode())
        bittorrent_->set_spider_ignore(!bittorrent_->is_spider_ignoring());

    engineLoop_.postDelayed([this] { onIgnoreToggle(); }, DEFAULT_IGNORE_INTERVAL_MS);
}

void Crawler::processMetadataQueue()
{
    if (!running_)
        return;

    while (activeFetches_.load() < MAX_CONCURRENT_METADATA_FETCHES) {
        MetadataRequest request;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (metadataQueue_.empty())
                break;
            request = metadataQueue_.front();
            metadataQueue_.pop();
        }
        fetchMetadata(request);
    }

    engineLoop_.postDelayed([this] { processMetadataQueue(); }, METADATA_QUEUE_INTERVAL_MS);
}

void Crawler::onAnnounce(const std::array<uint8_t, 20>& infoHash, const std::string& ip, uint16_t port)
{
    const std::string hashHex = toHex(infoHash);

    // De-duplicate: only announce and queue an info-hash the first time we see it.
    {
        std::lock_guard<std::mutex> lock(recentHashesMutex_);
        if (recentHashes_.count(hashHex) > 0)
            return;

        recentHashes_.insert(hashHex);

        // Bound the dedup set: drop roughly half once it grows too large.
        if (recentHashes_.size() > MAX_RECENT_HASHES) {
            auto it = recentHashes_.begin();
            std::advance(it, recentHashes_.size() / 2);
            recentHashes_.erase(recentHashes_.begin(), it);
        }
    }

    // Skip torrents we already have: fetching BEP 9 metadata for them would
    // burn a fetch slot and bandwidth only for the indexer to discard the
    // result as a duplicate. The caller injects the "already indexed?"
    // lookup, which keeps the crawler index-free.
    if (knownHashFilter_ && knownHashFilter_(hashHex)) {
        std::cout << "Crawler: already indexed " << hashHex.substr(0, 8) << ", skipping\n";
        return;
    }

    std::cout << "Crawler: announce " << hashHex.substr(0, 8) << " from " << ip << ":" << port << "\n";

    MetadataRequest request;
    request.infoHash = hashHex;
    request.peerIp = ip;
    request.peerPort = port;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        metadataQueue_.push(std::move(request));
    }
}

void Crawler::fetchMetadata(const MetadataRequest& request)
{
    if (!bittorrent_)
        return;

    activeFetches_++;

    const std::string infoHash = request.infoHash;
    const std::string peerIp = request.peerIp;
    const uint16_t peerPort = request.peerPort;
    const bool fastPath = !peerIp.empty() && peerPort > 0;

    // librats manages the temporary torrent, the BEP 9 fetch and the
    // timeout; it invokes this callback exactly once (success or timeout) on
    // a worker thread -- std::cout here is diagnostic only, not marshalled.
    auto onResult = [this, infoHash, fastPath](const librats::bittorrent::TorrentInfo& torrentInfo, bool success, const std::string& error) {
        activeFetches_--;

        if (!success || !torrentInfo.is_valid()) {
            // A fast-path failure only proves the announcing peer was
            // unhelpful (gone, firewalled, no ut_metadata support) -- the
            // info-hash itself is still good, and announces are the scarce
            // resource. Re-queue it once for the DHT-search slow path
            // instead of dropping it; the Qt app just drops these
            // (deliberate improvement over the port source). A slow-path
            // failure arrives here with fastPath false and falls through, so
            // each announce is retried at most once by construction.
            if (fastPath && running_) {
                std::cout << "Crawler: metadata fetch failed for " << infoHash.substr(0, 8) << " (" << error
                          << "), retrying via DHT search\n";
                MetadataRequest retry;
                retry.infoHash = infoHash;
                std::lock_guard<std::mutex> lock(queueMutex_);
                metadataQueue_.push(std::move(retry));
            } else {
                std::cout << "Crawler: metadata fetch failed for " << infoHash.substr(0, 8) << ": " << error << "\n";
            }
            return;
        }

        const domain::Torrent torrent = toDomainTorrent(infoHash, torrentInfo);
        engineLoop_.post([this, torrent] { onMetadataReceived(torrent); });
    };

    std::cout << "Crawler: fetching metadata for " << infoHash.substr(0, 8) << (fastPath ? " (fast path)\n" : " (DHT search)\n");
    if (fastPath) {
        // Fast path: fetch directly from the announcing peer (no DHT search).
        bittorrent_->get_torrent_metadata_from_peer(infoHash, peerIp, peerPort, onResult);
    } else {
        // Slow path: let librats find peers via the DHT.
        bittorrent_->get_torrent_metadata(infoHash, onResult);
    }
}

void Crawler::onMetadataReceived(const domain::Torrent& torrent)
{
    std::cout << "Crawler: discovered " << torrent.hash.substr(0, 8) << " \"" << torrent.name << "\"\n";
    discoveredCount_++;
    if (discovered_)
        discovered_(torrent);
}

} // namespace ratsn::engine
