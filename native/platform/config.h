#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ratsn::platform {

// <datadir>/rats-native.json. Flat JSON, key names reused from rats-search's
// QSettings-backed config where the concept survives (see docs/DESIGN-native.md §8)
// so a future "import settings from rats-search" stays trivial.
struct FilterConfig {
    int64_t sizeMin = 0;
    int64_t sizeMax = 0;
    int maxFiles = 0;
    std::string namingRegExp;
    bool namingRegExpNegative = false;
    bool adultFilter = false;
    std::string contentType; // "", "video", ..., "application" (Software+Games)
};

struct Config {
    bool spider = true;
    // ms between DHT walk steps. 100 matches the Qt app's real, tuned default
    // (src/app/config_store.cpp) -- it shipped slower once and got a
    // dedicated migration (v2.0.19) specifically to bring old installs up to
    // 100ms, so this isn't an arbitrary choice.
    int walkInterval = 100;
    // Default matches the Qt app (src/app/config_store.cpp). A stable port
    // matters for the spider: announce inflow depends on remote routing
    // tables still pointing at us, and an ephemeral port (set 0 to get one)
    // resets that reputation on every restart.
    int dhtPort = 6881;
    // Gates NodeHost's PortMappingService/HolePunch subsystems (M4).
    bool upnp = true;
    bool holePunch = true;
    // Mesh listen port; Qt key "p2pPort". 0 collides with the Qt app's own
    // default on shared localhost testing -- see docs/M4-PLAN.md.
    int p2pPort = 4444;
    // Inbound peer cap; Qt key "p2pConnections" (clamped to [10,1000] there,
    // same here -- see Config::load).
    int maxPeers = 10;
    // Ask connected peers for random torrents on a timer (engine/replication.h).
    bool p2pReplication = true;
    // Answer other peers' randomTorrents requests. Qt forces this true
    // whenever p2pReplication is on (config_store.cpp ~148); same here.
    bool p2pReplicationServer = true;
    std::string downloadPath;
    std::vector<std::string> trackers;
    FilterConfig filters;
    bool safeSearch = false;
    bool fileIndex =
#if RATSN_FILE_INDEX_DEFAULT
        true;
#else
        false;
#endif

    // Loads from `path`, filling in defaults for absent/malformed keys. Never
    // throws: a missing or corrupt file yields default-constructed Config.
    static Config load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path) const;
};

} // namespace ratsn::platform
