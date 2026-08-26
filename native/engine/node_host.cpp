#include "engine/node_host.h"

#include "engine/peer_registry.h"

#include "librats/nat/port_mapping.h"
#include "librats/node/node.h"
#include "librats/subsystems/bittorrent.h"
#include "librats/subsystems/dht_discovery.h"
#include "librats/subsystems/hole_punch.h"
#include "librats/subsystems/message_json.h"
#include "librats/subsystems/peer_exchange.h"
#include "librats/subsystems/port_mapping_service.h"
#include "librats/subsystems/reconnection.h"
#ifdef RATS_STORAGE
#include "librats/storage/storage.h"
#endif

#ifndef RATS_SEARCH_FEATURES
#error "ratsn's crawler (M2) needs librats built with -DRATS_SEARCH_FEATURES=ON. \
See native/CMakeLists.txt (RATSN_USE_SYSTEM_LIBRATS): the vendored-submodule \
path sets this automatically; a system-installed librats package must have \
been built with it too."
#endif

namespace ratsn::engine {

NodeHost::NodeHost(const platform::Config& cfg, std::filesystem::path dataDir, platform::EngineLoop& engineLoop,
    std::string clientVersion)
    : cfg_(cfg)
    , dataDir_(std::move(dataDir))
    , engineLoop_(engineLoop)
    , clientVersion_(std::move(clientVersion))
{
}

NodeHost::~NodeHost()
{
    stop();
}

bool NodeHost::start()
{
    if (running_)
        return true;

    librats::NodeConfig config;
    config.listen_port = static_cast<uint16_t>(cfg_.p2pPort);
    config.max_peers = cfg_.maxPeers > 0 ? static_cast<size_t>(cfg_.maxPeers) : 0;
    // Kept identical to src/net/p2p_transport.cpp's protocol id (version-less
    // so peers across patch releases meet); also namespaces DhtDiscovery's
    // own discovery hash.
    config.protocol = "rats-search/3";
    config.data_dir = dataDir_.string();
    config.security = librats::NodeConfig::Security::Noise;

    node_ = std::make_unique<librats::Node>(std::move(config));

    // DHT discovery (shared by the BitTorrent subsystem's spider mode).
    {
        librats::DhtDiscovery::Config dhtCfg;
        dhtCfg.dht_port = static_cast<uint16_t>(cfg_.dhtPort);
        dhtCfg.data_dir = dataDir_.string();
        dht_ = node_->add_subsystem(std::make_unique<librats::DhtDiscovery>(std::move(dhtCfg)));
    }

    // BitTorrent (DHT spider + BEP9 metadata fetch). Attached after
    // DhtDiscovery so it borrows the same Kademlia swarm.
    {
        librats::Bittorrent::Config btCfg;
        btCfg.client.download_path = dataDir_.string();
        btCfg.client.listen_port = static_cast<uint16_t>(cfg_.dhtPort);
        btCfg.use_node_dht = true;
        bittorrent_ = node_->add_subsystem(std::make_unique<librats::Bittorrent>(std::move(btCfg)));
    }

    // Typed JSON messaging: the on()/send() surface PeerApi and Replication
    // build on.
    messages_ = node_->add_subsystem(std::make_unique<librats::MessageJson>());

    // Remember + re-dial known peers across restarts.
    {
        librats::ReconnectionService::Config rc;
        rc.store_path = (dataDir_ / "peers.json").string();
        reconnect_ = node_->add_subsystem(std::make_unique<librats::ReconnectionService>(std::move(rc)));
    }

    // Automatic NAT port forwarding, gated by config (Qt key: upnp).
    if (cfg_.upnp) {
        librats::PortMappingConfig pmCfg;
        pmCfg.enabled = true;
        pmCfg.enable_upnp = true;
        pmCfg.enable_natpmp = true;
        portMapping_ = node_->add_subsystem(std::make_unique<librats::PortMappingService>(pmCfg));
    }

    // NAT hole punching, gated by config (Qt key: holePunch). Relaying other
    // peers' rendezvous is on: a mesh in which nobody relays cannot punch at
    // all, and one rendezvous costs a few dozen forwarded bytes to a peer we
    // already hold (matches p2p_transport.cpp).
    if (cfg_.holePunch) {
        librats::HolePunch::Config hpCfg;
        hpCfg.enable_relay = true;
        holePunch_ = node_->add_subsystem(std::make_unique<librats::HolePunch>(std::move(hpCfg)));
    }

    // Peer exchange: bounded by the same connection budget as everything
    // else -- NodeConfig::max_peers only refuses inbound peers, and PEX is
    // the one discovery source that compounds (matches p2p_transport.cpp).
    {
        librats::PeerExchange::Config pexCfg;
        pexCfg.peer_target = cfg_.maxPeers > 0 ? static_cast<size_t>(cfg_.maxPeers) : 0;
        pex_ = node_->add_subsystem(std::make_unique<librats::PeerExchange>(std::move(pexCfg)));
    }

    // Distributed key/value store (votes, M7). Gated on librats having been
    // built with RATS_STORAGE (default OFF there) -- see docs/M7-PLAN.md's
    // build prerequisite; without it, votes degrade to local-only counts
    // (P2PStore::isAvailable() reports false).
#ifdef RATS_STORAGE
    {
        librats::StorageConfig sc;
        sc.data_directory = (dataDir_ / "storage").string();
        storage_ = node_->add_subsystem(std::make_unique<librats::StorageManager>(sc));
    }
#endif

    // client_info handshake: must be wired before node_->start() per
    // node.h's documented contract on Node::on_peer_connected/disconnected.
    peerRegistry_ = std::make_unique<PeerRegistry>(*node_, *messages_, engineLoop_, clientVersion_);

    if (!node_->start()) {
        node_.reset();
        dht_ = nullptr;
        bittorrent_ = nullptr;
        messages_ = nullptr;
        reconnect_ = nullptr;
        portMapping_ = nullptr;
        holePunch_ = nullptr;
        pex_ = nullptr;
        storage_ = nullptr;
        peerRegistry_.reset();
        return false;
    }

    running_ = true;
    return true;
}

void NodeHost::stop()
{
    if (!running_)
        return;

    if (node_)
        node_->stop();
    node_.reset();
    dht_ = nullptr;
    bittorrent_ = nullptr;
    messages_ = nullptr;
    reconnect_ = nullptr;
    portMapping_ = nullptr;
    holePunch_ = nullptr;
    pex_ = nullptr;
    storage_ = nullptr;
    peerRegistry_.reset();
    running_ = false;
}

bool NodeHost::isDhtRunning() const
{
    return dht_ && dht_->is_running();
}

size_t NodeHost::dhtNodeCount() const
{
    if (!dht_)
        return 0;
    librats::DhtClient* client = dht_->dht_client();
    return client ? client->get_routing_table_size() : 0;
}

size_t NodeHost::spiderPoolSize() const
{
    return bittorrent_ ? bittorrent_->spider_pool_size() : 0;
}

size_t NodeHost::spiderVisitedCount() const
{
    return bittorrent_ ? bittorrent_->spider_visited_count() : 0;
}

std::string NodeHost::nodeIdShort() const
{
    return node_ ? node_->local_id().short_hex() : std::string();
}

std::string NodeHost::ourPeerId() const
{
    return node_ ? node_->local_id().to_hex() : std::string();
}

size_t NodeHost::peerCount() const
{
    return node_ ? node_->peer_count() : 0;
}

void NodeHost::connectTo(const std::string& host, uint16_t port)
{
    if (node_)
        node_->connect(host, port);
}

uint16_t NodeHost::listenPort() const
{
    return node_ ? node_->listen_port() : 0;
}

} // namespace ratsn::engine
