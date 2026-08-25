#pragma once

#include "domain/peer.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace librats {
class Node;
class MessageJson;
}

namespace ratsn::platform {
class EngineLoop;
}

namespace ratsn::engine {

// The rats-search peer handshake: on every new mesh connection, exchange a
// "client_info" message carrying our index stats, and track each peer's
// advertised stats so a future Status tab can show swarm-wide totals. Port of
// src/services/peer_registry.{h,cpp}.
//
// Registers librats::Node::on_peer_connected/on_peer_disconnected, which per
// node.h's documented contract must happen before Node::start(). That is why
// NodeHost constructs this itself, internally, before calling node_->start()
// (see node_host.cpp) -- unlike PeerApi and Replication, built only on
// MessageJson::on()/send(), which are documented thread-safe at any time and
// so have no such ordering constraint.
class PeerRegistry {
public:
    // node, messages and engineLoop are borrowed (non-owning) and must
    // outlive this object.
    PeerRegistry(librats::Node& node, librats::MessageJson& messages, platform::EngineLoop& engineLoop,
        std::string clientVersion);

    // Our own advertised stats; call when the index totals change.
    // EngineLoop-thread only.
    void updateOurStats(int64_t torrents, int64_t files, int64_t totalSize);

    // EngineLoop-thread only: every mutation of peers_ is itself marshalled
    // there (see the .cpp), so no locking is needed.
    const std::unordered_map<std::string, domain::PeerStats>& connectedPeers() const { return peers_; }
    int64_t remoteTorrentsCount() const;

    // Fires (on the EngineLoop thread) right after client_info is sent to a
    // newly connected peer. PeerApi hooks its own connect-time follow-up
    // (the replication kick) through this instead of registering its own
    // Node-level callback, which would hit the same before-start() ordering
    // constraint this class exists to satisfy.
    using PeerConnectedCallback = std::function<void(const std::string& peerIdHex)>;
    void setPeerConnectedCallback(PeerConnectedCallback cb) { onPeerConnectedExternal_ = std::move(cb); }

private:
    void onPeerConnected(const std::string& peerIdHex);
    void onPeerDisconnected(const std::string& peerIdHex);
    void onClientInfo(const std::string& peerIdHex, domain::PeerStats stats);
    domain::PeerStats ourStats() const;

    librats::Node& node_;
    librats::MessageJson& messages_;
    platform::EngineLoop& engineLoop_;
    std::string clientVersion_;
    int64_t ourTorrents_ = 0;
    int64_t ourFiles_ = 0;
    int64_t ourTotalSize_ = 0;

    std::unordered_map<std::string, domain::PeerStats> peers_;
    PeerConnectedCallback onPeerConnectedExternal_;
};

} // namespace ratsn::engine
