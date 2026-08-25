#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

namespace librats {
class MessageJson;
}

namespace ratsn::platform {
class EngineLoop;
}

namespace ratsn::engine {

// Periodically asks connected peers for random torrents so the local index
// converges toward the swarm's. Port of the timer/adaptive-interval policy in
// src/services/replication_service.{h,cpp} -- owns only that policy and the
// broadcast; the randomTorrents/randomTorrents_response wire handling lives
// in PeerApi, which calls notifyReceived() as torrents arrive.
//
// Reimplemented on EngineLoop::postDelayed rather than a restartable QTimer
// (self-rescheduling, same pattern as Crawler's timers): performCycle()
// broadcasts and then schedules settle() (which adapts interval_ for the
// *next* cycle) rather than pre-scheduling the next cycle before its own
// settle has run. This changes exactly when a mid-flight interval change
// takes effect, not the observable wire cadence the Qt original targets
// (broadcast roughly every interval_ ms, adapting on how much came back).
class Replication {
public:
    using PeerCountFn = std::function<size_t()>;

    // messages and engineLoop are borrowed (non-owning) and must outlive this
    // object. peerCount is injected (rather than this class reaching into
    // NodeHost itself) to keep its dependencies to exactly what the Qt
    // original had (P2PTransport::peerCount()).
    Replication(librats::MessageJson& messages, platform::EngineLoop& engineLoop, PeerCountFn peerCount);

    // Config gate (p2pReplication). Turning it off also stops the loop.
    void setEnabled(bool enabled);
    bool isEnabled() const { return enabled_; }

    void start();
    void stop();
    bool isRunning() const { return running_; }

    // Called by PeerApi each time a replicated torrent is inserted.
    void notifyReceived() { ++receivedThisCycle_; }

    int64_t totalReplicated() const { return totalReplicated_; }

private:
    void performCycle();
    void settle();

    librats::MessageJson& messages_;
    platform::EngineLoop& engineLoop_;
    PeerCountFn peerCount_;

    bool enabled_ = false;
    bool running_ = false;
    int interval_;
    // Reset at the start of every cycle; drives the adaptive interval.
    std::atomic<int> receivedThisCycle_ { 0 };
    int64_t totalReplicated_ = 0;

    static constexpr int kInitialIntervalMs = 5000;
    static constexpr int kIdleIntervalMs = 10000;
    static constexpr int kMaxIntervalMs = 60000;
    static constexpr int kSettleDelayMs = 3000; // wait for peer replies before adapting
    static constexpr int kBusyThreshold = 8; // received above this => back off
    static constexpr int kBackoffPerTorrentMs = 600;
    static constexpr int kTorrentsPerPeer = 5;
};

} // namespace ratsn::engine
