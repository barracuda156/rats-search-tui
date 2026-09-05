#include "engine/peer_registry.h"

#include "platform/engine_loop.h"
#include "platform/log.h"

#include "librats/core/types.h"  // CloseReason, to_string
#include "librats/node/node.h"
#include "librats/peer/peer.h"
#include "librats/peer/peer_id.h"
#include "librats/subsystems/message_json.h"

#include <chrono>

namespace ratsn::engine {

namespace {
int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

PeerRegistry::PeerRegistry(librats::Node& node, librats::MessageJson& messages, platform::EngineLoop& engineLoop,
    std::string clientVersion)
    : node_(node)
    , messages_(messages)
    , engineLoop_(engineLoop)
    , clientVersion_(std::move(clientVersion))
{
    node_.on_peer_connected([this](const librats::Peer& peer) {
        std::string peerIdHex = peer.id().to_hex();
        engineLoop_.post([this, peerIdHex] { onPeerConnected(peerIdHex); });
    });
    node_.on_peer_disconnected([this](const librats::PeerId& id, librats::CloseReason reason) {
        std::string peerIdHex = id.to_hex();
        // to_string returns a static string literal, so it is safe to carry
        // across the hop to the EngineLoop thread by pointer.
        const char* reasonText = librats::to_string(reason);
        engineLoop_.post([this, peerIdHex, reasonText] { onPeerDisconnected(peerIdHex, reasonText); });
    });
    messages_.on("client_info", [this](const librats::PeerId& from, const librats::Json& data) {
        std::string peerIdHex = from.to_hex();
        domain::PeerStats stats = domain::PeerStats::fromJson(data);
        engineLoop_.post([this, peerIdHex, stats] { onClientInfo(peerIdHex, stats); });
    });
}

void PeerRegistry::updateOurStats(int64_t torrents, int64_t files, int64_t totalSize)
{
    ourTorrents_ = torrents;
    ourFiles_ = files;
    ourTotalSize_ = totalSize;
}

domain::PeerStats PeerRegistry::ourStats() const
{
    domain::PeerStats s;
    s.clientVersion = clientVersion_;
    s.torrents = ourTorrents_;
    s.files = ourFiles_;
    s.totalSize = ourTotalSize_;
    s.peersConnected = static_cast<int>(node_.peer_count());
    return s;
}

void PeerRegistry::onPeerConnected(const std::string& peerIdHex)
{
    // Introduce ourselves; the peer replies with its own client_info.
    if (const auto id = librats::PeerId::from_hex(peerIdHex))
        messages_.send(*id, "client_info", ourStats().toJson());

    if (onPeerConnectedExternal_)
        onPeerConnectedExternal_(peerIdHex);
}

void PeerRegistry::onPeerDisconnected(const std::string& peerIdHex, const char* reason)
{
    // The reason distinguishes "the peer went away" from "we were dropped as a
    // slow consumer" -- see peer_network.h. Nothing acts on it yet; log it so a
    // SlowConsumer disconnect is not invisible.
    platform::log() << "PeerRegistry: peer " << peerIdHex.substr(0, 8) << " disconnected (" << reason << ")\n";
    peers_.erase(peerIdHex);
}

void PeerRegistry::onClientInfo(const std::string& peerIdHex, domain::PeerStats stats)
{
    stats.connectedAt = nowMs();
    peers_[peerIdHex] = stats;

    platform::log() << "PeerRegistry: peer " << peerIdHex.substr(0, 8) << " v" << stats.clientVersion
               << " torrents:" << stats.torrents << " files:" << stats.files << "\n";
}

int64_t PeerRegistry::remoteTorrentsCount() const
{
    int64_t total = 0;
    for (const auto& [id, stats] : peers_)
        total += stats.torrents;
    return total;
}

} // namespace ratsn::engine
