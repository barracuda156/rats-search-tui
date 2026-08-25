#pragma once

#include "engine/downloads.h" // for Download / DownloadFile

#include <string>
#include <vector>

namespace ratsn::engine {

// Reads and writes the download session file (torrents_session.json): the
// set of active downloads to restore on the next launch. Pure JSON file I/O
// -- no state, no librats calls (DownloadManager asks the client to persist
// resume data separately, see saveSession). Port of
// src/services/torrent_session_store.{h,cpp}.
//
// Serialisation reuses DownloadFile::toJson(), the very same helper a future
// live-progress path would use, so the persisted per-file shape cannot drift
// from it. Never hand-build the file object here.
class TorrentSessionStore {
public:
    // Overwrite the session file with `downloads`. Removes the file when the
    // list is empty. Returns false only on a real write error.
    bool save(const std::string& filePath, const std::vector<Download>& downloads) const;

    // Parse the session file into restore entries (empty on a missing or
    // malformed file). Populated fields: hash, name, savePath, totalSize,
    // paused, removeOnDone, completed, downloadedBytes, progress and the
    // per-file selection/progress.
    std::vector<Download> load(const std::string& filePath) const;
};

} // namespace ratsn::engine
