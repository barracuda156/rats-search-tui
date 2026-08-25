#include "engine/session_store.h"

#include "platform/log.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace ratsn::engine {

bool TorrentSessionStore::save(const std::string& filePath, const std::vector<Download>& downloads) const
{
    if (downloads.empty()) {
        std::error_code ec;
        std::filesystem::remove(filePath, ec);
        return true;
    }

    librats::Json sessionsArray = librats::Json::array();
    for (const Download& d : downloads) {
        librats::Json session = librats::Json::object();
        session["hash"] = d.hash;
        session["name"] = d.name;
        session["savePath"] = d.savePath;
        session["totalSize"] = d.totalSize;
        session["paused"] = d.paused;
        session["removeOnDone"] = d.removeOnDone;
        session["completed"] = d.completed; // completed torrents are kept for seeding
        session["downloadedBytes"] = d.downloadedBytes;
        session["progress"] = d.progress;

        // Reuse the shared file->JSON helper so the persisted per-file shape
        // (including `progress`) matches DownloadFile's own toJson.
        librats::Json filesArr = librats::Json::array();
        for (const DownloadFile& f : d.files)
            filesArr.push_back(f.toJson());
        session["files"] = std::move(filesArr);

        sessionsArray.push_back(std::move(session));
    }

    const std::filesystem::path path(filePath);
    std::error_code dirEc;
    std::filesystem::create_directories(path.parent_path(), dirEc);

    // Write-to-temp-then-rename: a save interrupted mid-write (crash, power
    // loss -- exactly the situation the 60s autosave exists for, see
    // DownloadManager::autosaveTick) must never leave a half-written session
    // file behind for the next launch to choke on.
    const std::filesystem::path tmp = filePath + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            platform::log() << "TorrentSessionStore: failed to save session to " << filePath << "\n";
            return false;
        }
        out << sessionsArray.dump(2);
        if (!out) {
            platform::log() << "TorrentSessionStore: failed to write session to " << filePath << "\n";
            return false;
        }
    }
    std::error_code renameEc;
    std::filesystem::rename(tmp, path, renameEc);
    if (renameEc) {
        platform::log() << "TorrentSessionStore: failed to finalize session file " << filePath << "\n";
        return false;
    }

    platform::log() << "TorrentSessionStore: saved " << downloads.size() << " torrents to session file\n";
    return true;
}

std::vector<Download> TorrentSessionStore::load(const std::string& filePath) const
{
    std::vector<Download> entries;

    std::ifstream in(filePath, std::ios::binary);
    if (!in)
        return entries;

    std::ostringstream buf;
    buf << in.rdbuf();

    librats::Json doc = librats::Json::parse(buf.str(), nullptr, false);
    if (doc.is_discarded() || !doc.is_array()) {
        platform::log() << "TorrentSessionStore: failed to parse session file: " << filePath << "\n";
        return entries;
    }

    for (const librats::Json& val : doc) {
        if (!val.is_object())
            continue;

        Download d;
        d.hash = val.value("hash", std::string());
        d.name = val.value("name", std::string());
        d.savePath = val.value("savePath", std::string());
        d.totalSize = val.value("totalSize", int64_t { 0 });
        d.paused = val.value("paused", false);
        d.removeOnDone = val.value("removeOnDone", false);
        d.completed = val.value("completed", false);
        d.downloadedBytes = val.value("downloadedBytes", int64_t { 0 });
        d.progress = val.value("progress", 0.0);

        if (val.contains("files") && val["files"].is_array()) {
            int i = 0;
            for (const librats::Json& fileVal : val["files"]) {
                DownloadFile f;
                f.path = fileVal.value("path", std::string());
                f.size = fileVal.value("size", int64_t { 0 });
                f.index = fileVal.value("index", i);
                f.selected = fileVal.value("selected", true);
                f.progress = fileVal.value("progress", 0.0);
                d.files.push_back(std::move(f));
                ++i;
            }
        }

        entries.push_back(std::move(d));
    }

    return entries;
}

} // namespace ratsn::engine
