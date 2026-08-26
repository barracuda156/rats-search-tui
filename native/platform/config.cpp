#include "platform/config.h"

#include "librats/util/json.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace ratsn::platform {

namespace {

constexpr int kMinMaxPeers = 10;
constexpr int kMaxMaxPeers = 1000;

librats::Json filtersToJson(const FilterConfig& f)
{
    librats::Json j = librats::Json::object();
    j["sizeMin"] = f.sizeMin;
    j["sizeMax"] = f.sizeMax;
    j["maxFiles"] = f.maxFiles;
    j["namingRegExp"] = f.namingRegExp;
    j["namingRegExpNegative"] = f.namingRegExpNegative;
    j["adultFilter"] = f.adultFilter;
    j["contentType"] = f.contentType;
    return j;
}

FilterConfig filtersFromJson(const librats::Json& j)
{
    FilterConfig f;
    if (!j.is_object())
        return f;
    f.sizeMin = j.value("sizeMin", int64_t { 0 });
    f.sizeMax = j.value("sizeMax", int64_t { 0 });
    f.maxFiles = j.value("maxFiles", 0);
    f.namingRegExp = j.value("namingRegExp", "");
    f.namingRegExpNegative = j.value("namingRegExpNegative", false);
    f.adultFilter = j.value("adultFilter", false);
    f.contentType = j.value("contentType", "");
    return f;
}

} // namespace

Config Config::load(const std::filesystem::path& path)
{
    Config cfg;

    std::ifstream in(path, std::ios::binary);
    if (!in)
        return cfg;

    std::ostringstream buf;
    buf << in.rdbuf();
    librats::Json j = librats::Json::parse(buf.str(), nullptr, false);
    if (!j.is_object())
        return cfg;

    cfg.spider = j.value("spider", cfg.spider);
    cfg.walkInterval = j.value("walkInterval", cfg.walkInterval);
    cfg.dhtPort = j.value("dhtPort", cfg.dhtPort);
    cfg.upnp = j.value("upnp", cfg.upnp);
    cfg.holePunch = j.value("holePunch", cfg.holePunch);
    cfg.p2pPort = j.value("p2pPort", cfg.p2pPort);
    cfg.maxPeers = j.value("p2pConnections", cfg.maxPeers);
    cfg.p2pReplication = j.value("p2pReplication", cfg.p2pReplication);
    cfg.p2pReplicationServer = j.value("p2pReplicationServer", cfg.p2pReplicationServer);
    cfg.downloadPath = j.value("downloadPath", cfg.downloadPath);
    cfg.safeSearch = j.value("safeSearch", cfg.safeSearch);
    cfg.strictSearch = j.value("strictSearch", cfg.strictSearch);
    cfg.trackerRequireKnown = j.value("trackerRequireKnown", cfg.trackerRequireKnown);
    // "trackers" used to be (mistakenly, and unused by any engine code) an
    // array of tracker URLs here; it's now Qt's actual "trackers" key, a
    // bool. A datadir carrying the old array shape from a pre-M8 native
    // build must not crash Config::load's "never throws" contract -- Json::
    // get<bool>() throws on a non-boolean value, so check the type first and
    // fall back to the default (matches every other key's "malformed ->
    // default" handling here).
    if (const librats::Json* trackersFlag = j.as_object().find("trackers"); trackersFlag && trackersFlag->is_boolean())
        cfg.trackersEnabled = trackersFlag->get<bool>();
    cfg.siteScraper = j.value("siteScraper", cfg.siteScraper);
    cfg.indexMaxTorrents = j.value("indexMaxTorrents", cfg.indexMaxTorrents);
    cfg.fileIndex = j.value("fileIndex", cfg.fileIndex);

    // Same clamp/repair rules as Qt's ConfigStore::validateAndClamp: an
    // out-of-range or hand-edited-inconsistent value must never reach the
    // services.
    cfg.maxPeers = std::clamp(cfg.maxPeers, kMinMaxPeers, kMaxMaxPeers);
    if (cfg.p2pReplication && !cfg.p2pReplicationServer)
        cfg.p2pReplicationServer = true;
    if (cfg.indexMaxTorrents < 0)
        cfg.indexMaxTorrents = 0;

    if (const librats::Json* allow = j.as_object().find("trackerAllow"); allow && allow->is_array()) {
        cfg.trackerAllow.clear();
        for (const auto& t : *allow)
            cfg.trackerAllow.push_back(t.get<std::string>());
    }
    if (const librats::Json* deny = j.as_object().find("trackerDeny"); deny && deny->is_array()) {
        cfg.trackerDeny.clear();
        for (const auto& t : *deny)
            cfg.trackerDeny.push_back(t.get<std::string>());
    }

    if (const librats::Json* filters = j.as_object().find("filters"); filters)
        cfg.filters = filtersFromJson(*filters);

    return cfg;
}

bool Config::save(const std::filesystem::path& path) const
{
    librats::Json j = librats::Json::object();
    j["spider"] = spider;
    j["walkInterval"] = walkInterval;
    j["dhtPort"] = dhtPort;
    j["upnp"] = upnp;
    j["holePunch"] = holePunch;
    j["p2pPort"] = p2pPort;
    j["p2pConnections"] = maxPeers;
    j["p2pReplication"] = p2pReplication;
    j["p2pReplicationServer"] = p2pReplicationServer;
    j["downloadPath"] = downloadPath;
    j["safeSearch"] = safeSearch;
    j["strictSearch"] = strictSearch;
    j["trackerRequireKnown"] = trackerRequireKnown;
    j["indexMaxTorrents"] = indexMaxTorrents;
    j["fileIndex"] = fileIndex;
    j["trackers"] = trackersEnabled;
    j["siteScraper"] = siteScraper;

    librats::Json trackerAllowJson = librats::Json::array();
    for (const auto& t : trackerAllow)
        trackerAllowJson.push_back(t);
    j["trackerAllow"] = std::move(trackerAllowJson);

    librats::Json trackerDenyJson = librats::Json::array();
    for (const auto& t : trackerDeny)
        trackerDenyJson.push_back(t);
    j["trackerDeny"] = std::move(trackerDenyJson);

    j["filters"] = filtersToJson(filters);

    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << j.dump(2);
        if (!out)
            return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

} // namespace ratsn::platform
