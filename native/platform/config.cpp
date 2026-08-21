#include "platform/config.h"

#include "librats/util/json.h"

#include <fstream>
#include <sstream>

namespace ratsn::platform {

namespace {

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
    f.namingRegExpNegative = j.value("namingRegExpNegative", "");
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
    cfg.downloadPath = j.value("downloadPath", cfg.downloadPath);
    cfg.safeSearch = j.value("safeSearch", cfg.safeSearch);
    cfg.fileIndex = j.value("fileIndex", cfg.fileIndex);

    if (const librats::Json* trackers = j.as_object().find("trackers"); trackers && trackers->is_array()) {
        cfg.trackers.clear();
        for (const auto& t : *trackers)
            cfg.trackers.push_back(t.get<std::string>());
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
    j["downloadPath"] = downloadPath;
    j["safeSearch"] = safeSearch;
    j["fileIndex"] = fileIndex;

    librats::Json trackersJson = librats::Json::array();
    for (const auto& t : trackers)
        trackersJson.push_back(t);
    j["trackers"] = std::move(trackersJson);

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
