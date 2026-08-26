#pragma once

#include "librats/util/json.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace librats {
class StorageManager;
}

namespace ratsn::platform {
class EngineLoop;
}

namespace ratsn::engine {

class NodeHost;

// A record stored in the distributed P2P store. `data` is the full JSON
// payload as it lives in librats' StorageManager (including the injected
// `_key`/`_peerId`/`_timestamp` metadata fields).
struct StoredRecord {
    std::string key;
    std::string type;
    librats::Json data;
    std::string peerId; // peer that created this record
    int64_t timestamp = 0;
};

// Native port of src/services/p2p_store.{h,cpp} (docs/M7-PLAN.md item 2):
// wraps the librats distributed key-value StorageManager, borrowed from
// NodeHost. Provides put/find/has of JSON records that replicate across
// peers; all librats storage access is confined to this class so services
// above it (Voting) never touch librats directly. Behavior is gated on the
// RATS_STORAGE feature flag -- isAvailable() reports false when disabled or
// nodeHost is null/not running.
//
// The record contract (a `type` field plus the injected `_key`/`_peerId`/
// `_timestamp` metadata) and the key format are replicated across the swarm,
// so changing either stops existing records (including votes) from
// aggregating.
class P2PStore {
public:
    using Listener = std::function<void(const StoredRecord& record, bool isRemote)>;

    // nodeHost is borrowed and must outlive this object; may be null (e.g.
    // spider/mesh disabled), in which case isAvailable() always reports
    // false, mirroring Qt's behavior when the transport never came up.
    // engineLoop is borrowed and must outlive this object -- the remote
    // change callback fires on a librats reactor thread and is marshalled
    // onto it before the listener runs.
    P2PStore(NodeHost* nodeHost, platform::EngineLoop& engineLoop);
    ~P2PStore();

    P2PStore(const P2PStore&) = delete;
    P2PStore& operator=(const P2PStore&) = delete;

    bool isAvailable() const;
    std::string ourPeerId() const;

    // Store `obj` under an explicit key, injecting the internal metadata.
    // Callers manage their own key namespace (e.g. per-peer vote records).
    // `obj` should carry a "type" field.
    bool put(const std::string& key, librats::Json obj);

    // Find all records whose key starts with `indexPrefix`.
    std::vector<StoredRecord> find(const std::string& indexPrefix) const;

    bool has(const std::string& key) const;

    // A record was stored, either locally (isRemote == false, called
    // synchronously from put()) or received from a peer (isRemote == true,
    // already marshalled onto the EngineLoop thread). Single listener --
    // Voting is the sole consumer (docs/M7-PLAN.md item 2), unlike Qt's
    // Qt-signal fan-out.
    void setListener(Listener listener) { listener_ = std::move(listener); }

private:
    void setupStorageCallbacks();
    librats::StorageManager* storage() const;

    NodeHost* nodeHost_;
    platform::EngineLoop& engineLoop_;
    Listener listener_;
};

} // namespace ratsn::engine
