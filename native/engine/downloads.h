#pragma once

#include "domain/torrent.h"
#include "librats/util/json.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace librats {
namespace bittorrent {
class Client;
struct TorrentStatus;
}
}

namespace ratsn::platform {
class EngineLoop;
}

namespace ratsn::engine {

class NodeHost;
class TorrentSessionStore;

// Per-file download state -- extends the pure path+size pair with the
// download-runtime bits. Port of src/services/download_service.h's
// DownloadFile; field names copied verbatim (see toJson) so
// torrents_session.json stays byte-compatible with the Qt app's.
struct DownloadFile {
    std::string path;
    int64_t size = 0;
    int index = 0;
    bool selected = true;
    double progress = 0.0;

    librats::Json toJson() const;
};

// The live state of one active download (or a completed torrent kept for
// seeding). Port of src/services/download_service.h's Download.
struct Download {
    std::string hash;
    std::string name;
    std::string savePath;
    int64_t totalSize = 0;
    int64_t downloadedBytes = 0;
    double progress = 0.0;
    double downloadSpeed = 0.0;
    int peersConnected = 0;
    bool paused = false;
    bool removeOnDone = false;
    bool ready = false; // metadata (name/files/size) known
    bool completed = false;
    std::vector<DownloadFile> files;

    // Rolling sample used to derive downloadSpeed from the cumulative byte
    // counter between polls (librats exposes total bytes, not a per-torrent
    // rate). Poll-internal; not persisted (see toJson).
    int64_t lastSampledBytes = 0;
    int64_t lastSampledMs = 0;

    // Last progress payload computed, so the 1s poll can tell whether a
    // download's displayed state actually changed (idle/paused/seeding
    // downloads otherwise look "changed" every tick just from the poll
    // running) -- see DownloadManager::revision(). Poll-internal; not
    // persisted.
    librats::Json lastProgress;

    librats::Json toJson() const;
};

// Owns the active-download registry and the 1s progress-poll/60s-autosave
// timers. All state is confined to the EngineLoop thread (docs/M6-PLAN.md
// item 2): callers reach it only via EngineLoop::post, so unlike Qt's
// DownloadService this holds no mutex. Speaks librats::bittorrent::Client
// directly (native has no separate TorrentEngine wrapper layer) but keeps
// every librats type out of its own public interface bar the fully-qualified
// forward declarations above.
//
// Note on failure reporting: there is deliberately no async "download
// failed" notification. A magnet whose metadata never arrives is retried by
// librats indefinitely, so the only failures are the synchronous ones -- bad
// hash, engine not ready, add_* rejected -- reported by the bool return of
// the add/restore methods (mirrors Qt's DownloadService).
class DownloadManager {
public:
    // nodeHost/engineLoop are borrowed (non-owning) and must outlive this
    // object. nodeHost is nullable (spider/mesh disabled): every operation
    // then reports "not ready", same as the Qt service's isReady() check.
    DownloadManager(NodeHost* nodeHost, platform::EngineLoop& engineLoop, std::string defaultDownloadPath);
    ~DownloadManager();

    DownloadManager(const DownloadManager&) = delete;
    DownloadManager& operator=(const DownloadManager&) = delete;

    // Begins the 1s progress poll and the 60s session autosave (deliberate
    // deviation from Qt, which only persists on clean shutdown -- see
    // docs/M6-PLAN.md "Deliberate deviations" #2). sessionFilePath is where
    // the autosave writes; pass the same path given to loadSession.
    void start(std::string sessionFilePath);
    // Stops both timers. Does not itself save -- callers do one final
    // explicit saveSession() before tearing down the node (main.cpp mirrors
    // application.cpp's shutdown ordering).
    void stop();

    // --- Download lifecycle -------------------------------------------------
    bool add(const std::string& magnetOrHash, const std::string& savePath = {});
    bool addWithInfo(const domain::Torrent& info, const std::string& savePath = {});
    bool addFromFile(const std::string& torrentFile, const std::string& savePath = {});

    // Stop and remove a torrent, keeping its downloaded files on disk.
    // saveResumeData preserves the downloaded pieces for a later re-add.
    void remove(const std::string& hash, bool saveResumeData = false);
    // Stop, remove and delete every file the torrent owns (the TUI's double
    // 'X' -- docs/M6-PLAN.md deviation #3). librats' own
    // Client::remove_torrent delete_files flag is not implemented yet (see
    // client.cpp: "File deletion is not yet implemented"), so this deletes
    // the files itself from the last-known file list + save path.
    void removeAndDelete(const std::string& hash);
    bool pause(const std::string& hash);
    bool resume(const std::string& hash);
    bool togglePause(const std::string& hash);

    // --- Queries (engine-thread only, like every other member here) --------
    bool isDownloading(const std::string& hash) const;
    Download getDownload(const std::string& hash) const;
    std::vector<Download> snapshot() const;

    // Bumped whenever a poll finds a download's displayed state changed (see
    // Download::lastProgress) or the registry itself changes (add/remove).
    // The TUI compares this against its last-seen value to skip a redraw
    // when nothing actually moved (docs/M6-PLAN.md item 5's "1s updater ...
    // idle churn costs redraws").
    uint64_t revision() const { return revision_; }

    struct Aggregate {
        int active = 0; // downloading, i.e. ready, not paused, not completed
        double downloadSpeed = 0.0; // sum of active downloads' bytes/sec
    };
    Aggregate aggregate() const;

    void setDefaultDownloadPath(std::string path) { defaultDownloadPath_ = std::move(path); }

    // --- Session persistence -------------------------------------------------
    bool saveSession(const std::string& filePath);
    int loadSession(const std::string& filePath);

    // Fires once a download completes (engine-thread; the TUI marshals to
    // the UI thread itself via screen_.Post, same as every other engine
    // callback -- see ResultView::handleSaveTorrent for the idiom).
    using CompletionCallback = std::function<void(const std::string& hash, const std::string& name)>;
    void setCompletionCallback(CompletionCallback cb) { onCompleted_ = std::move(cb); }

private:
    librats::bittorrent::Client* client() const;
    bool isReady() const;

    // Restore a torrent from a previous session, loading any resume data.
    // The persisted entry seeds the live state (notably `completed`) so a
    // finished torrent shows as completed immediately, before librats' async
    // recheck.
    bool restore(const Download& entry);
    void setRemoveOnDone(const std::string& hash, bool removeOnDone);

    // State changes detected during a poll, flushed after the loop below.
    struct Transitions {
        std::vector<std::string> newlyReady;
        std::vector<std::string> newlyCompleted;
        std::vector<std::string> toRemove;
    };

    void tick(); // self-rescheduling 1s poll (EngineLoop::postDelayed)
    Transitions pollStatus();
    void computeSpeed(Download& d, int64_t nowMs);
    void flushTransitions(const Transitions& t);
    void applySnapshot(Download& d, const librats::bittorrent::TorrentStatus& st);

    void autosaveTick(); // self-rescheduling 60s autosave

    std::string resolveSavePath(const std::string& savePath) const;
    static bool ensureDir(const std::string& path);
    bool contains(const std::string& hash) const;
    static librats::Json progressJson(const Download& d);

    NodeHost* nodeHost_; // borrowed, non-owning
    platform::EngineLoop& engineLoop_; // borrowed, non-owning
    std::unique_ptr<TorrentSessionStore> sessionStore_;
    std::string defaultDownloadPath_;
    std::string sessionFilePath_;

    std::map<std::string, Download> downloads_;
    bool running_ = false;
    uint64_t revision_ = 0;
    uint64_t lastSavedRevision_ = 0;
    bool everSaved_ = false;

    CompletionCallback onCompleted_;
};

} // namespace ratsn::engine
