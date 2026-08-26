#include "engine/p2p_store.h"

#include "engine/node_host.h"
#include "platform/engine_loop.h"
#include "platform/log.h"

#include <chrono>
#include <optional>

#ifdef RATS_STORAGE
#include "librats/storage/storage.h"
#endif

namespace ratsn::engine {

namespace {
int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

P2PStore::P2PStore(NodeHost* nodeHost, platform::EngineLoop& engineLoop) : nodeHost_(nodeHost), engineLoop_(engineLoop)
{
    setupStorageCallbacks();
    platform::log() << "P2PStore: initialized\n";
}

P2PStore::~P2PStore() = default;

librats::StorageManager* P2PStore::storage() const
{
    return nodeHost_ ? nodeHost_->storage() : nullptr;
}

bool P2PStore::isAvailable() const
{
#ifdef RATS_STORAGE
    return storage() != nullptr;
#else
    return false;
#endif
}

std::string P2PStore::ourPeerId() const
{
    return nodeHost_ ? nodeHost_->ourPeerId() : std::string();
}

bool P2PStore::put(const std::string& key, librats::Json obj)
{
#ifdef RATS_STORAGE
    librats::StorageManager* s = storage();
    if (!s) {
        platform::log() << "P2PStore: storage not available\n";
        return false;
    }

    obj["_key"] = key;
    obj["_peerId"] = ourPeerId();
    obj["_timestamp"] = nowMs();

    const bool result = s->put_json(key, obj);
    if (result) {
        StoredRecord record;
        record.key = key;
        record.type = obj.value("type", "");
        record.data = obj;
        record.peerId = ourPeerId();
        record.timestamp = nowMs();
        if (listener_)
            listener_(record, /*isRemote*/ false);
    }
    return result;
#else
    (void)key;
    (void)obj;
    platform::log() << "P2PStore: storage feature not enabled (RATS_STORAGE not defined)\n";
    return false;
#endif
}

std::vector<StoredRecord> P2PStore::find(const std::string& indexPrefix) const
{
    std::vector<StoredRecord> results;
#ifdef RATS_STORAGE
    librats::StorageManager* s = storage();
    if (!s)
        return results;

    for (const std::string& key : s->keys_with_prefix(indexPrefix)) {
        const std::optional<librats::Json> json = s->get_json(key);
        if (!json)
            continue;
        StoredRecord record;
        record.key = key;
        record.type = json->value("type", "");
        record.peerId = json->value("_peerId", "");
        record.timestamp = json->value("_timestamp", int64_t { 0 });
        record.data = *json;
        results.push_back(std::move(record));
    }
#else
    (void)indexPrefix;
#endif
    return results;
}

bool P2PStore::has(const std::string& key) const
{
#ifdef RATS_STORAGE
    librats::StorageManager* s = storage();
    return s && s->has(key);
#else
    (void)key;
    return false;
#endif
}

void P2PStore::setupStorageCallbacks()
{
#ifdef RATS_STORAGE
    librats::StorageManager* s = storage();
    if (!s)
        return;

    s->set_change_callback([this](const librats::StorageChangeEvent& event) {
        if (!event.is_remote)
            return;

        const std::string jsonStr(event.new_data.begin(), event.new_data.end());
        librats::Json data = librats::Json::parse(jsonStr, nullptr, false);
        if (data.is_discarded())
            return;

        StoredRecord record;
        record.key = event.key;
        record.type = data.value("type", "");
        record.peerId = event.origin_peer_id;
        record.timestamp = static_cast<int64_t>(event.timestamp_ms);
        record.data = std::move(data);

        // Fires on a librats reactor thread -- marshal onto the EngineLoop
        // thread before the listener touches any app state (the native
        // translation of Qt's queued signal/slot connection).
        engineLoop_.post([this, record = std::move(record)] {
            if (listener_)
                listener_(record, /*isRemote*/ true);
        });
    });

    s->set_sync_complete_callback([](bool success, const std::string& error) {
        platform::log() << "P2PStore: sync completed, success=" << (success ? "true" : "false")
                         << (error.empty() ? "" : (", error: " + error)) << "\n";
    });
#endif
}

} // namespace ratsn::engine
