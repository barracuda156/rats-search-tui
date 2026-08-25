#pragma once

#include "platform/config.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace librats {
class Node;
class DhtDiscovery;
class Bittorrent;
class MessageJson;
class ReconnectionService;
class PeerExchange;
class PortMappingService;
class HolePunch;
}

namespace ratsn::platform {
class EngineLoop;
}

namespace ratsn::engine {

class PeerRegistry;

// Owns the librats Node for both the DHT-crawl + BEP9-metadata pipeline (M2)
// and, as of M4, the P2P mesh: MessageJson (the on()/send() surface PeerApi
// and Replication build on), ReconnectionService, PeerExchange, and the
// config-gated PortMappingService/HolePunch. Config values are ported from
// src/net/p2p_transport.cpp's node setup (see docs/M4-PLAN.md).
//
// MdnsDiscovery and StorageManager remain unattached (see
// docs/DESIGN-native.md §4/§10): LAN discovery isn't needed for M4's
// acceptance check (a debug --connect flag substitutes, since DHT-based
// mesh discovery is slow/flaky for a 2-node lab -- see main.cpp), and
// StorageManager is a genuine M6 (votes) concern.
class NodeHost {
public:
    // engineLoop is borrowed (non-owning) and must outlive this object --
    // needed to marshal the mesh's peer-connect/client_info callbacks onto
    // the EngineLoop thread (see PeerRegistry). clientVersion is advertised
    // to peers in the client_info handshake.
    NodeHost(const platform::Config& cfg, std::filesystem::path dataDir, platform::EngineLoop& engineLoop,
        std::string clientVersion);
    ~NodeHost();

    NodeHost(const NodeHost&) = delete;
    NodeHost& operator=(const NodeHost&) = delete;

    bool start();
    void stop();
    bool isRunning() const { return running_; }

    // Borrowed (non-owning); valid only while running.
    librats::Bittorrent* bittorrent() const { return bittorrent_; }
    librats::Node* node() const { return node_.get(); }
    librats::MessageJson* messageJson() const { return messages_; }
    PeerRegistry* peerRegistry() const { return peerRegistry_.get(); }

    size_t peerCount() const;
    std::string ourPeerId() const; // full hex; nodeIdShort() below is for display

    // Debug-only localhost pairing (docs/M4-PLAN.md "Localhost acceptance
    // setup"): dial an explicit peer instead of relying on DHT/PEX discovery.
    // No-op if not running.
    void connectTo(const std::string& host, uint16_t port);

    bool isDhtRunning() const;
    size_t dhtNodeCount() const;

    // Short hex prefix of the node's self-certifying identity, and its bound
    // listen port (the actual port when the config requested 0) -- both real
    // regardless of M2's scope (Node is always up for DHT), unlike NAT/peer
    // counts which need the mesh subsystems (M4+). Empty/0 when not running.
    std::string nodeIdShort() const;
    uint16_t listenPort() const;

    // Spider-mode progress, independent of whether any metadata fetch has
    // completed yet -- pool is targets queued to visit, visited is targets
    // walked so far. Both climbing with discoveredCount() stuck at 0 means
    // the walk is progressing but hasn't produced an announce yet (give it
    // more time); pool/visited themselves stuck at 0 would point at spider
    // mode not actually being active.
    size_t spiderPoolSize() const;
    size_t spiderVisitedCount() const;

private:
    platform::Config cfg_;
    std::filesystem::path dataDir_;
    platform::EngineLoop& engineLoop_;
    std::string clientVersion_;

    std::unique_ptr<librats::Node> node_;
    librats::DhtDiscovery* dht_ = nullptr;
    librats::Bittorrent* bittorrent_ = nullptr;
    librats::MessageJson* messages_ = nullptr;
    librats::ReconnectionService* reconnect_ = nullptr;
    librats::PeerExchange* pex_ = nullptr;
    librats::PortMappingService* portMapping_ = nullptr;
    librats::HolePunch* holePunch_ = nullptr;
    std::unique_ptr<PeerRegistry> peerRegistry_;
    bool running_ = false;
};

} // namespace ratsn::engine
