#include "engine/replication.h"

#include "platform/engine_loop.h"

#include "librats/subsystems/message_json.h"
#include "librats/util/json.h"

#include <algorithm>
#include <iostream>

namespace ratsn::engine {

Replication::Replication(librats::MessageJson& messages, platform::EngineLoop& engineLoop, PeerCountFn peerCount)
    : messages_(messages)
    , engineLoop_(engineLoop)
    , peerCount_(std::move(peerCount))
    , interval_(kInitialIntervalMs)
{
}

void Replication::setEnabled(bool enabled)
{
    enabled_ = enabled;
    if (!enabled_)
        stop();
}

void Replication::start()
{
    if (!enabled_ || running_)
        return;
    interval_ = kInitialIntervalMs;
    receivedThisCycle_ = 0;
    running_ = true;
    std::cout << "Replication: started, interval " << interval_ << "ms\n";
    engineLoop_.postDelayed([this] { performCycle(); }, interval_);
}

void Replication::stop()
{
    if (!running_)
        return;
    running_ = false;
    std::cout << "Replication: stopped, total replicated: " << totalReplicated_ << "\n";
}

void Replication::performCycle()
{
    if (!running_)
        return;
    if (!enabled_) {
        stop();
        return;
    }

    if (peerCount_() == 0) {
        engineLoop_.postDelayed([this] { performCycle(); }, interval_);
        return;
    }

    receivedThisCycle_ = 0;
    librats::Json data = librats::Json::object();
    data["limit"] = kTorrentsPerPeer;
    data["version"] = std::string("2.0");
    messages_.send("randomTorrents", data);

    engineLoop_.postDelayed([this] { settle(); }, kSettleDelayMs);
}

void Replication::settle()
{
    if (!running_)
        return;

    const int received = receivedThisCycle_.load();
    interval_ = received > kBusyThreshold ? std::min(kMaxIntervalMs, received * kBackoffPerTorrentMs) : kIdleIntervalMs;
    if (received > 0) {
        totalReplicated_ += received;
        std::cout << "Replication: +" << received << " torrents, total " << totalReplicated_ << ", next " << interval_
                   << "ms\n";
    }

    engineLoop_.postDelayed([this] { performCycle(); }, interval_);
}

} // namespace ratsn::engine
