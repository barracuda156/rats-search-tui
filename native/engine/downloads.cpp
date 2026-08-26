#include "engine/downloads.h"

#include "engine/node_host.h"
#include "engine/session_store.h"
#include "platform/engine_loop.h"
#include "platform/log.h"

#include "librats/bittorrent/client.h"
#include "librats/bittorrent/torrent_info.h"
#include "librats/bittorrent/types.h"
#include "librats/subsystems/bittorrent.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <optional>
#include <system_error>

namespace ratsn::engine {

namespace {
namespace bt = librats::bittorrent;

bool isValidHash(const std::string& h)
{
    if (h.size() != 40)
        return false;
    return std::all_of(h.begin(), h.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string normalizeHash(std::string h)
{
    for (char& c : h)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h;
}

std::string asciiLower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Extract a 40-char lower-case info-hash from a magnet URI or a raw hash.
// Returns an empty string when none can be parsed. Port of
// TorrentEngine::parseInfoHash (src/net/torrent_engine.cpp) without
// QRegularExpression.
std::string parseInfoHash(const std::string& magnetOrHash)
{
    if (isValidHash(magnetOrHash))
        return normalizeHash(magnetOrHash);

    const std::string lower = asciiLower(magnetOrHash);
    const std::string needle = "btih:";
    const size_t pos = lower.find(needle);
    if (pos == std::string::npos)
        return {};

    const size_t start = pos + needle.size();
    if (start + 40 <= magnetOrHash.size()) {
        const std::string candidate = magnetOrHash.substr(start, 40);
        if (isValidHash(candidate))
            return normalizeHash(candidate);
    }

    // Base32-encoded hashes (32 chars) are recognised only so the caller gets
    // a diagnostic instead of a silent "no hash"; decoding them is not
    // implemented (matches TorrentEngine::parseInfoHash).
    if (start + 32 <= magnetOrHash.size()) {
        const std::string candidate32 = magnetOrHash.substr(start, 32);
        const bool base32 = std::all_of(candidate32.begin(), candidate32.end(), [](unsigned char c) {
            c = static_cast<unsigned char>(std::toupper(c));
            return (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7');
        });
        if (base32)
            platform::log() << "DownloadManager: base32-encoded info-hashes are not supported\n";
    }
    return {};
}

std::optional<bt::InfoHash> toInfoHash(const std::string& hex)
{
    return bt::info_hash_from_hex(hex);
}

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

// ============================================================================
// DownloadFile / Download -- JSON
// ============================================================================

librats::Json DownloadFile::toJson() const
{
    librats::Json obj = librats::Json::object();
    obj["path"] = path;
    obj["size"] = size;
    obj["index"] = index;
    obj["selected"] = selected;
    obj["progress"] = progress;
    return obj;
}

librats::Json Download::toJson() const
{
    librats::Json obj = librats::Json::object();
    obj["hash"] = hash;
    obj["name"] = name;
    obj["savePath"] = savePath;
    obj["totalSize"] = totalSize;
    obj["downloadedBytes"] = downloadedBytes;
    obj["progress"] = progress;
    obj["downloadSpeed"] = downloadSpeed;
    obj["peersConnected"] = peersConnected;
    obj["paused"] = paused;
    obj["removeOnDone"] = removeOnDone;
    obj["ready"] = ready;
    obj["completed"] = completed;

    librats::Json filesArr = librats::Json::array();
    for (const DownloadFile& f : files)
        filesArr.push_back(f.toJson());
    obj["files"] = std::move(filesArr);
    return obj;
}

// ============================================================================
// Construction
// ============================================================================

DownloadManager::DownloadManager(NodeHost* nodeHost, platform::EngineLoop& engineLoop, std::string defaultDownloadPath)
    : nodeHost_(nodeHost)
    , engineLoop_(engineLoop)
    , sessionStore_(std::make_unique<TorrentSessionStore>())
    , defaultDownloadPath_(std::move(defaultDownloadPath))
{
}

DownloadManager::~DownloadManager() { stop(); }

librats::bittorrent::Client* DownloadManager::client() const
{
    librats::Bittorrent* bt2 = nodeHost_ ? nodeHost_->bittorrent() : nullptr;
    return bt2 ? bt2->client() : nullptr;
}

bool DownloadManager::isReady() const { return client() != nullptr; }

void DownloadManager::start(std::string sessionFilePath)
{
    sessionFilePath_ = std::move(sessionFilePath);
    if (running_)
        return;
    running_ = true;
    engineLoop_.postDelayed([this] { tick(); }, 1000);
    engineLoop_.postDelayed([this] { autosaveTick(); }, 60000);
}

void DownloadManager::stop() { running_ = false; }

// ============================================================================
// Download lifecycle
// ============================================================================

bool DownloadManager::add(const std::string& magnetOrHash, const std::string& savePath)
{
    if (!isReady()) {
        platform::log() << "DownloadManager: not ready\n";
        return false;
    }

    std::string hash = parseInfoHash(magnetOrHash);
    if (!isValidHash(hash)) {
        platform::log() << "DownloadManager: invalid magnet link or hash: " << magnetOrHash << "\n";
        return false;
    }
    hash = normalizeHash(hash);

    if (contains(hash)) {
        platform::log() << "DownloadManager: already downloading: " << hash.substr(0, 8) << "\n";
        return false;
    }

    const std::string path = resolveSavePath(savePath);
    ensureDir(path);

    platform::log() << "DownloadManager: adding torrent " << hash.substr(0, 8) << " to " << path << "\n";
    auto* download = client()->add_magnet("magnet:?xt=urn:btih:" + hash, path);
    if (!download) {
        platform::log() << "DownloadManager: failed to add magnet: " << hash.substr(0, 8) << "\n";
        return false;
    }

    Download d;
    d.hash = hash;
    d.name = hash; // placeholder until metadata arrives
    d.savePath = path;
    d.ready = false;
    downloads_[hash] = std::move(d);
    ++revision_;
    return true;
}

bool DownloadManager::addWithInfo(const domain::Torrent& info, const std::string& savePath)
{
    if (!isValidHash(info.hash)) {
        platform::log() << "DownloadManager: invalid hash for download: " << info.hash << "\n";
        return false;
    }
    if (!isReady()) {
        platform::log() << "DownloadManager: not ready\n";
        return false;
    }

    const std::string hash = normalizeHash(info.hash);
    if (contains(hash)) {
        platform::log() << "DownloadManager: already downloading: " << hash.substr(0, 8) << "\n";
        return false;
    }

    const std::string path = resolveSavePath(savePath);
    ensureDir(path);

    platform::log() << "DownloadManager: adding torrent with info " << hash.substr(0, 8) << " \""
                     << info.name.substr(0, 50) << "\"\n";
    auto* download = client()->add_magnet("magnet:?xt=urn:btih:" + hash, path);
    if (!download) {
        platform::log() << "DownloadManager: failed to add magnet: " << hash.substr(0, 8) << "\n";
        return false;
    }

    Download d;
    d.hash = hash;
    d.name = info.name.empty() ? hash : info.name;
    d.totalSize = info.size;
    d.savePath = path;
    d.ready = false; // metadata (authoritative name/files) not yet known
    d.completed = false;
    downloads_[hash] = std::move(d);
    ++revision_;
    return true;
}

bool DownloadManager::addFromFile(const std::string& torrentFile, const std::string& savePath)
{
    if (!isReady()) {
        platform::log() << "DownloadManager: not ready\n";
        return false;
    }

    auto info = bt::TorrentInfo::from_file(torrentFile);
    if (!info || !info->is_valid()) {
        platform::log() << "DownloadManager: failed to parse torrent file: " << torrentFile << "\n";
        return false;
    }

    const std::string hash = normalizeHash(info->info_hash_hex());
    if (contains(hash)) {
        platform::log() << "DownloadManager: already downloading: " << hash.substr(0, 8) << "\n";
        return false;
    }

    const std::string path = resolveSavePath(savePath);
    ensureDir(path);

    platform::log() << "DownloadManager: adding torrent file " << torrentFile << " to " << path << "\n";
    auto* download = client()->add_torrent(*info, path);
    if (!download) {
        platform::log() << "DownloadManager: failed to add torrent file: " << torrentFile << "\n";
        return false;
    }

    Download d;
    d.hash = hash;
    d.name = info->name();
    d.savePath = path;
    d.totalSize = info->total_size();
    d.ready = true;
    const auto& fileList = info->files().files();
    for (size_t i = 0; i < fileList.size(); ++i) {
        DownloadFile f;
        f.path = fileList[i].path;
        f.size = static_cast<int64_t>(fileList[i].size);
        f.index = static_cast<int>(i);
        f.selected = true;
        d.files.push_back(std::move(f));
    }
    downloads_[hash] = std::move(d);
    ++revision_;
    return true;
}

bool DownloadManager::restore(const Download& entry)
{
    if (!isReady()) {
        platform::log() << "DownloadManager: not ready for restore\n";
        return false;
    }

    const std::string h = normalizeHash(entry.hash);
    if (contains(h)) {
        platform::log() << "DownloadManager: already active: " << h.substr(0, 8) << "\n";
        return false;
    }

    const std::string path = resolveSavePath(entry.savePath);
    ensureDir(path);

    platform::log() << "DownloadManager: restoring torrent " << h.substr(0, 8) << " \"" << entry.name.substr(0, 30)
                     << "\" from " << path << "\n";
    auto* download = client()->add_magnet_resumed("magnet:?xt=urn:btih:" + h, path);
    if (!download) {
        platform::log() << "DownloadManager: failed to restore torrent: " << h.substr(0, 8) << "\n";
        return false;
    }

    Download d;
    d.hash = h;
    d.savePath = path;
    d.name = entry.name.empty() ? h : entry.name;
    d.ready = false;

    // Seed the live state from the persisted session. librats rechecks the
    // on-disk files asynchronously -- torrent_status() right now still
    // reports 0% / not-complete -- so without this a finished torrent would
    // flash as a 0% download until the recheck lands. The 1s poll stays
    // authoritative and reconciles once the check completes. paused/
    // removeOnDone are applied by loadSession() afterwards, so they are
    // deliberately not seeded here.
    d.completed = entry.completed;
    d.totalSize = entry.totalSize;
    d.downloadedBytes = entry.downloadedBytes;
    d.progress = entry.progress;
    d.files = entry.files;

    // If resume data already brought back the metadata, populate immediately.
    if (auto ih = toInfoHash(h)) {
        bt::TorrentStatus st = client()->torrent_status(*ih);
        if (st.exists && st.has_metadata) {
            const bool completedBefore = d.completed;
            applySnapshot(d, st);
            // applySnapshot mirrors librats' *pre-recheck* view (is_complete
            // still false, progress 0), so it must not downgrade a torrent
            // the session recorded as complete.
            if (completedBefore && !d.completed) {
                d.completed = true;
                d.progress = 1.0;
                d.downloadedBytes = d.totalSize;
                for (DownloadFile& f : d.files)
                    f.progress = 1.0;
            }
        }
    }

    downloads_[h] = std::move(d);
    ++revision_;
    return true;
}

namespace {
// Deletes the files a torrent owns and any now-empty parent directories
// (deepest first) -- librats' Client::remove_torrent delete_files flag is a
// no-op today (bittorrent/client.cpp: "File deletion is not yet
// implemented"), so DownloadManager::removeAndDelete does the work itself.
void deleteDownloadFiles(const Download& d)
{
    if (d.savePath.empty())
        return;

    const std::filesystem::path base(d.savePath);
    const std::string& basePrefix = base.native();
    const auto underSavePath = [&basePrefix](const std::filesystem::path& p) {
        return p.native().size() > basePrefix.size()
            && p.native().compare(0, basePrefix.size(), basePrefix) == 0;
    };

    std::error_code ec;
    std::vector<std::filesystem::path> dirs;
    for (const DownloadFile& f : d.files) {
        const std::filesystem::path full = base / f.path;
        // operator/ replaces the base outright when f.path is absolute --
        // librats' combine_paths never does (it string-joins), so the
        // written file always lands under savePath and a full path that
        // doesn't must not be deleted (a hostile torrent's file list is
        // attacker-controlled).
        if (!underSavePath(full))
            continue;
        std::filesystem::remove(full, ec);
        // Every ancestor strictly below savePath, not just the immediate
        // parent: a torrent whose files all live in subdirectories would
        // otherwise leave its now-empty root directory behind.
        for (std::filesystem::path parent = full.parent_path(); underSavePath(parent); parent = parent.parent_path())
            dirs.push_back(parent);
    }

    // Descending lexicographic order puts every child before its parent (a
    // chain of now-empty subdirectories collapses in one pass) and groups
    // duplicates for unique(); remove() (not remove_all) refuses anything
    // still non-empty, so a directory shared with other content is left
    // alone.
    std::sort(dirs.begin(), dirs.end(),
        [](const std::filesystem::path& a, const std::filesystem::path& b) { return b < a; });
    dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
    for (const std::filesystem::path& dir : dirs)
        std::filesystem::remove(dir, ec);
}
} // namespace

void DownloadManager::remove(const std::string& hash, bool saveResumeData)
{
    const std::string h = normalizeHash(hash);

    auto it = downloads_.find(h);
    if (it == downloads_.end()) {
        platform::log() << "DownloadManager: torrent not found: " << h.substr(0, 8) << "\n";
        return;
    }
    downloads_.erase(it);
    ++revision_;

    if (auto* c = client()) {
        if (auto ih = toInfoHash(h)) {
            if (saveResumeData) {
                platform::log() << "DownloadManager: saving resume data for: " << h.substr(0, 8) << "\n";
                c->save_resume_data(*ih);
            }
            c->remove_torrent(*ih);
        }
    }

    platform::log() << "DownloadManager: stopped and removed torrent: " << h.substr(0, 8) << "\n";
}

void DownloadManager::removeAndDelete(const std::string& hash)
{
    const std::string h = normalizeHash(hash);

    auto it = downloads_.find(h);
    if (it == downloads_.end()) {
        platform::log() << "DownloadManager: torrent not found: " << h.substr(0, 8) << "\n";
        return;
    }
    Download removed = std::move(it->second);
    downloads_.erase(it);
    ++revision_;

    if (auto* c = client()) {
        if (auto ih = toInfoHash(h))
            c->remove_torrent(*ih, /*delete_files=*/true);
    }

    deleteDownloadFiles(removed);
    platform::log() << "DownloadManager: removed and deleted files for: " << h.substr(0, 8) << "\n";
}

bool DownloadManager::pause(const std::string& hash)
{
    const std::string h = normalizeHash(hash);
    auto it = downloads_.find(h);
    if (it == downloads_.end() || it->second.paused)
        return false;
    it->second.paused = true;
    ++revision_;

    if (auto* c = client()) {
        if (auto ih = toInfoHash(h))
            c->pause_torrent(*ih);
    }
    return true;
}

bool DownloadManager::resume(const std::string& hash)
{
    const std::string h = normalizeHash(hash);
    auto it = downloads_.find(h);
    if (it == downloads_.end() || !it->second.paused)
        return false;
    it->second.paused = false;
    ++revision_;

    if (auto* c = client()) {
        if (auto ih = toInfoHash(h))
            c->resume_torrent(*ih);
    }
    return true;
}

bool DownloadManager::togglePause(const std::string& hash)
{
    const std::string h = normalizeHash(hash);
    auto it = downloads_.find(h);
    if (it == downloads_.end())
        return false;
    return it->second.paused ? resume(h) : pause(h);
}

void DownloadManager::setRemoveOnDone(const std::string& hash, bool removeOnDone)
{
    auto it = downloads_.find(normalizeHash(hash));
    if (it != downloads_.end())
        it->second.removeOnDone = removeOnDone;
}

// ============================================================================
// Queries
// ============================================================================

bool DownloadManager::isDownloading(const std::string& hash) const { return contains(normalizeHash(hash)); }

Download DownloadManager::getDownload(const std::string& hash) const
{
    auto it = downloads_.find(normalizeHash(hash));
    return it == downloads_.end() ? Download {} : it->second;
}

std::vector<Download> DownloadManager::snapshot() const
{
    std::vector<Download> result;
    result.reserve(downloads_.size());
    for (const auto& [hash, d] : downloads_)
        result.push_back(d);
    return result;
}

DownloadManager::Aggregate DownloadManager::aggregate() const
{
    Aggregate agg;
    for (const auto& [hash, d] : downloads_) {
        if (d.ready && !d.paused && !d.completed) {
            ++agg.active;
            agg.downloadSpeed += d.downloadSpeed;
        }
    }
    return agg;
}

// ============================================================================
// Session persistence
// ============================================================================

bool DownloadManager::saveSession(const std::string& filePath)
{
    std::vector<Download> snap = snapshot();

    // Ask librats to persist resume data (downloaded pieces) for each torrent.
    if (auto* c = client()) {
        for (const Download& d : snap) {
            platform::log() << "DownloadManager: saving resume data for " << d.hash.substr(0, 8) << "\n";
            if (auto ih = toInfoHash(d.hash))
                c->save_resume_data(*ih);
        }
    }

    return sessionStore_->save(filePath, snap);
}

int DownloadManager::loadSession(const std::string& filePath)
{
    std::vector<Download> entries = sessionStore_->load(filePath);
    int restored = 0;

    for (const Download& e : entries) {
        if (!isValidHash(e.hash))
            continue;

        platform::log() << "DownloadManager: restoring torrent: " << e.hash.substr(0, 8) << " \""
                         << e.name.substr(0, 30) << "\" " << (e.completed ? "(completed/seeding)" : "(downloading)")
                         << "\n";

        if (restore(e)) {
            if (e.paused)
                pause(e.hash);
            setRemoveOnDone(e.hash, e.removeOnDone);

            if (!e.files.empty()) {
                auto it = downloads_.find(normalizeHash(e.hash));
                if (it != downloads_.end()) {
                    for (size_t i = 0; i < e.files.size() && i < it->second.files.size(); ++i)
                        it->second.files[i].selected = e.files[i].selected;
                }
            }
            ++restored;
        }
    }

    if (restored > 0)
        platform::log() << "DownloadManager: restored " << restored << " torrents from session\n";
    return restored;
}

// ============================================================================
// Progress polling
// ============================================================================

void DownloadManager::tick()
{
    if (!running_)
        return;
    if (isReady()) {
        Transitions t = pollStatus();
        flushTransitions(t);
    }
    engineLoop_.postDelayed([this] { tick(); }, 1000);
}

DownloadManager::Transitions DownloadManager::pollStatus()
{
    Transitions t;
    auto* c = client();

    for (auto& [hash, d] : downloads_) {
        if (auto ih = toInfoHash(hash)) {
            bt::TorrentStatus st = c->torrent_status(*ih);
            if (!st.exists)
                continue; // librats no longer tracks it -- leave as-is

            const bool wasReady = d.ready;
            const bool wasCompleted = d.completed;

            if (st.has_metadata && !wasReady) {
                // Metadata just arrived for a magnet torrent -> populate details.
                applySnapshot(d, st);
                t.newlyReady.push_back(hash);
            } else {
                // Live counters only.
                d.downloadedBytes = static_cast<int64_t>(st.downloaded);
                d.peersConnected = static_cast<int>(st.num_peers);
                d.progress = st.progress;
                if (d.totalSize == 0 && st.total_size > 0)
                    d.totalSize = static_cast<int64_t>(st.total_size);
            }

            computeSpeed(d, nowMs());

            if (st.is_complete && !wasCompleted) {
                d.completed = true;
                d.progress = 1.0;
                d.downloadSpeed = 0.0;
                t.newlyCompleted.push_back(hash);
                if (d.removeOnDone)
                    t.toRemove.push_back(hash);
            }

            const librats::Json progress = progressJson(d);
            // Only count as "changed" when the displayed snapshot actually
            // moved -- an idle, paused or finished-seeding download
            // otherwise bumps revision_ (and the TUI's redraw with it) every
            // second for nothing.
            if (!(progress == d.lastProgress)) {
                d.lastProgress = progress;
                ++revision_;
            }
        }
    }

    return t;
}

void DownloadManager::computeSpeed(Download& d, int64_t nowMsVal)
{
    if (d.lastSampledMs > 0 && nowMsVal > d.lastSampledMs) {
        const int64_t deltaBytes = d.downloadedBytes - d.lastSampledBytes;
        const double deltaSec = static_cast<double>(nowMsVal - d.lastSampledMs) / 1000.0;
        d.downloadSpeed = deltaBytes > 0 ? static_cast<double>(deltaBytes) / deltaSec : 0.0;
    }
    d.lastSampledBytes = d.downloadedBytes;
    d.lastSampledMs = nowMsVal;
}

void DownloadManager::flushTransitions(const Transitions& t)
{
    for (const std::string& hash : t.newlyReady)
        platform::log() << "DownloadManager: metadata received for: " << hash.substr(0, 8) << "\n";

    for (const std::string& hash : t.newlyCompleted) {
        platform::log() << "DownloadManager: download completed: " << hash.substr(0, 8) << "\n";
        if (onCompleted_) {
            auto it = downloads_.find(hash);
            onCompleted_(hash, it != downloads_.end() ? it->second.name : hash);
        }
    }

    for (const std::string& hash : t.toRemove)
        remove(hash);
}

void DownloadManager::autosaveTick()
{
    if (!running_)
        return;
    if (!sessionFilePath_.empty() && (!everSaved_ || revision_ != lastSavedRevision_)) {
        if (saveSession(sessionFilePath_)) {
            lastSavedRevision_ = revision_;
            everSaved_ = true;
        }
    }
    engineLoop_.postDelayed([this] { autosaveTick(); }, 60000);
}

// ============================================================================
// Private helpers
// ============================================================================

void DownloadManager::applySnapshot(Download& d, const librats::bittorrent::TorrentStatus& st)
{
    if (!st.name.empty())
        d.name = st.name;
    d.totalSize = static_cast<int64_t>(st.total_size);
    d.downloadedBytes = static_cast<int64_t>(st.downloaded);
    d.progress = st.progress;
    d.peersConnected = static_cast<int>(st.num_peers);
    d.completed = st.is_complete;
    d.ready = st.has_metadata;

    d.files.clear();
    for (size_t i = 0; i < st.files.size(); ++i) {
        DownloadFile f;
        f.path = st.files[i].path;
        f.size = static_cast<int64_t>(st.files[i].size);
        f.index = static_cast<int>(i);
        f.selected = true;
        if (st.is_complete)
            f.progress = 1.0;
        d.files.push_back(std::move(f));
    }
}

std::string DownloadManager::resolveSavePath(const std::string& savePath) const
{
    return savePath.empty() ? defaultDownloadPath_ : savePath;
}

bool DownloadManager::ensureDir(const std::string& path)
{
    std::error_code ec;
    if (std::filesystem::exists(path, ec))
        return true;
    return std::filesystem::create_directories(path, ec);
}

bool DownloadManager::contains(const std::string& hash) const { return downloads_.count(hash) > 0; }

librats::Json DownloadManager::progressJson(const Download& d)
{
    librats::Json o = librats::Json::object();
    o["downloaded"] = d.downloadedBytes;
    o["total"] = d.totalSize;
    o["progress"] = d.progress;
    o["downloadSpeed"] = static_cast<int64_t>(d.downloadSpeed);
    o["paused"] = d.paused;
    o["completed"] = d.completed;
    o["removeOnDone"] = d.removeOnDone;
    if (d.downloadSpeed > 0 && d.totalSize > d.downloadedBytes)
        o["timeRemaining"] = static_cast<int64_t>(static_cast<double>(d.totalSize - d.downloadedBytes) / d.downloadSpeed);
    else
        o["timeRemaining"] = static_cast<int64_t>(0);
    return o;
}

} // namespace ratsn::engine
