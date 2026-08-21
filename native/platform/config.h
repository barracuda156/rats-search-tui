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
    std::string namingRegExpNegative;
    bool adultFilter = false;
    std::string contentType; // "", "video", ..., "application" (Software+Games)
};

struct Config {
    bool spider = true;
    int walkInterval = 1000; // ms between DHT walk steps
    int dhtPort = 0; // 0 = let librats pick
    bool upnp = true;
    bool holePunch = true;
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
