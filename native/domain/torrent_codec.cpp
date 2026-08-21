#include "domain/torrent_codec.h"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace ratsn::domain::codec {

namespace {
std::string toLowerHex(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

librats::Json filesToJson(const std::vector<File>& files)
{
    librats::Json array = librats::Json::array();
    for (const File& f : files) {
        librats::Json obj = librats::Json::object();
        obj["path"] = f.path;
        obj["size"] = f.size;
        array.push_back(std::move(obj));
    }
    return array;
}

std::vector<File> filesFromJson(const librats::Json& array)
{
    std::vector<File> files;
    if (!array.is_array())
        return files;
    files.reserve(array.size());
    for (const librats::Json& v : array) {
        File f;
        f.path = v.value("path", "");
        f.size = v.value("size", int64_t { 0 });
        files.push_back(std::move(f));
    }
    return files;
}

librats::Json toJson(const Torrent& t, ToJsonOptions options)
{
    librats::Json obj = librats::Json::object();
    obj["hash"] = t.hash;
    obj["name"] = t.name;
    obj["size"] = t.size;
    obj["files"] = t.files;
    obj["pieceLength"] = t.pieceLength;
    obj["added"] = t.added;
    obj["contentType"] = toString(t.contentType);
    obj["contentCategory"] = toString(t.contentCategory);
    obj["seeders"] = t.seeders;
    obj["leechers"] = t.leechers;
    obj["completed"] = t.completed;
    obj["trackersChecked"] = t.trackersChecked;
    obj["good"] = t.good;
    obj["bad"] = t.bad;

    if (options.includeInfo && !t.info.empty())
        obj["info"] = t.info;
    if (options.includeFiles)
        obj["files_list"] = filesToJson(t.fileList);

    return obj;
}

librats::Json toJson(const SearchHit& hit, ToJsonOptions options)
{
    librats::Json obj = toJson(hit.torrent, options);
    if (hit.fromFileMatch)
        obj["fileMatch"] = true;
    if (!hit.matchingPaths.empty()) {
        librats::Json paths = librats::Json::array();
        for (const std::string& p : hit.matchingPaths)
            paths.push_back(p);
        obj["matchingPaths"] = std::move(paths);
    }
    if (!hit.sourcePeerId.empty())
        obj["peer"] = hit.sourcePeerId;
    if (hit.remote)
        obj["remote"] = true;
    return obj;
}

Torrent torrentFromJson(const librats::Json& obj)
{
    Torrent t;
    if (!obj.is_object())
        return t;

    t.hash = toLowerHex(obj.value("hash", ""));
    if (t.hash.empty())
        t.hash = toLowerHex(obj.value("info_hash", "")); // legacy alias

    t.name = obj.value("name", "");
    t.size = obj.value("size", int64_t { 0 });
    t.files = obj.value("files", 0);
    t.pieceLength = obj.value("pieceLength", 0);
    t.seeders = obj.value("seeders", 0);
    t.leechers = obj.value("leechers", 0);
    t.completed = obj.value("completed", 0);
    t.good = obj.value("good", 0);
    t.bad = obj.value("bad", 0);

    const int64_t addedMs = obj.value("added", int64_t { 0 });
    t.added = addedMs > 0 ? addedMs : nowMs();
    t.trackersChecked = obj.value("trackersChecked", int64_t { 0 });

    t.contentType = contentTypeFromString(obj.value("contentType", ""));
    t.contentCategory = contentCategoryFromString(obj.value("contentCategory", ""));

    if (obj.contains("info"))
        t.info = *obj.as_object().find("info");

    if (obj.contains("files_list"))
        t.fileList = filesFromJson(*obj.as_object().find("files_list"));
    else if (obj.contains("filesList")) // legacy key
        t.fileList = filesFromJson(*obj.as_object().find("filesList"));

    if (t.files == 0 && !t.fileList.empty())
        t.files = static_cast<int>(t.fileList.size());

    return t;
}

SearchHit searchHitFromJson(const librats::Json& obj)
{
    SearchHit hit;
    hit.torrent = torrentFromJson(obj);
    hit.fromFileMatch = obj.value("fileMatch", false) || obj.value("isFileMatch", false);
    if (obj.is_object()) {
        if (const librats::Json* paths = obj.as_object().find("matchingPaths"); paths && paths->is_array()) {
            for (const librats::Json& v : *paths)
                hit.matchingPaths.push_back(v.get<std::string>());
        }
    }
    hit.sourcePeerId = obj.value("peer", "");
    hit.remote = obj.value("remote", false) || !hit.sourcePeerId.empty();
    return hit;
}

} // namespace ratsn::domain::codec
