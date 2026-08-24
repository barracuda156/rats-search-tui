#pragma once

#include "platform/config.h"

#include <cstddef>
#include <filesystem>
#include <memory>

namespace librats {
class Node;
class DhtDiscovery;
class Bittorrent;
}

namespace ratsn::engine {

// Owns the librats Node for the DHT-crawl + BEP9-metadata pipeline (M2).
// Attaches only DhtDiscovery (real Kademlia/Mainline DHT client) and
// Bittorrent (spider mode + metadata fetch, delegates to that same DHT).
//
// The P2P *mesh* subsystems -- MdnsDiscovery, MessageJson,
// PortMappingService, ReconnectionService, PeerExchange, HolePunch,
// StorageManager -- are M4/M5/M6 concerns (remote search, downloads, votes)
// and deliberately not attached here; see docs/DESIGN-native.md §4/§10 and
// the M2 plan. Config values for the two subsystems this does attach are
// ported from src/net/p2p_transport.cpp's node setup.
class NodeHost {
public:
    NodeHost(const platform::Config& cfg, std::filesystem::path dataDir);
    ~NodeHost();

    NodeHost(const NodeHost&) = delete;
    NodeHost& operator=(const NodeHost&) = delete;

    bool start();
    void stop();
    bool isRunning() const { return running_; }

    // Borrowed (non-owning); valid only while running.
    librats::Bittorrent* bittorrent() const { return bittorrent_; }

    bool isDhtRunning() const;
    size_t dhtNodeCount() const;

private:
    platform::Config cfg_;
    std::filesystem::path dataDir_;

    std::unique_ptr<librats::Node> node_;
    librats::DhtDiscovery* dht_ = nullptr;
    librats::Bittorrent* bittorrent_ = nullptr;
    bool running_ = false;
};

} // namespace ratsn::engine
