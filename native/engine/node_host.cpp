#include "engine/node_host.h"

#include "librats/node/node.h"
#include "librats/subsystems/bittorrent.h"
#include "librats/subsystems/dht_discovery.h"

#ifndef RATS_SEARCH_FEATURES
#error "ratsn's crawler (M2) needs librats built with -DRATS_SEARCH_FEATURES=ON. \
See native/CMakeLists.txt (RATSN_USE_SYSTEM_LIBRATS): the vendored-submodule \
path sets this automatically; a system-installed librats package must have \
been built with it too."
#endif

namespace ratsn::engine {

NodeHost::NodeHost(const platform::Config& cfg, std::filesystem::path dataDir)
    : cfg_(cfg)
    , dataDir_(std::move(dataDir))
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
    // No inbound P2P peers expected yet -- M2 attaches neither MessageJson
    // nor PeerExchange, so there is nothing to dial us for. Ephemeral port
    // is fine.
    config.listen_port = 0;
    config.max_peers = 0;
    // Kept identical to src/net/p2p_transport.cpp's protocol id (version-less
    // so peers across patch releases meet) even though M2 never joins the
    // peer mesh -- it also namespaces DhtDiscovery's own discovery hash.
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

    if (!node_->start()) {
        node_.reset();
        dht_ = nullptr;
        bittorrent_ = nullptr;
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

uint16_t NodeHost::listenPort() const
{
    return node_ ? node_->listen_port() : 0;
}

} // namespace ratsn::engine
