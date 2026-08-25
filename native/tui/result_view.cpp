#include "tui/result_view.h"

#include "domain/content.h"
#include "engine/downloads.h"
#include "engine/node_host.h"
#include "engine/torrent_file.h"
#include "platform/log.h"
#include "tui/format.h"

#include "librats/subsystems/bittorrent.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

using namespace ftxui;

namespace ratsn::tui {

ResultView::ResultView(platform::EngineLoop& engineLoop, ftxui::ScreenInteractive& screen, engine::NodeHost* nodeHost,
    engine::DownloadManager* downloads, std::string dataDir)
    : engineLoop_(engineLoop)
    , screen_(screen)
    , nodeHost_(nodeHost)
    , downloads_(downloads)
    , dataDir_(std::move(dataDir))
{
}

bool ResultView::hasHash(const std::string& hash) const
{
    for (const domain::SearchHit& existing : results_) {
        if (existing.torrent.hash == hash)
            return true;
    }
    return false;
}

void ResultView::setResults(std::vector<domain::SearchHit> hits)
{
    results_ = std::move(hits);
    resultLines_.clear();
    resultLines_.reserve(results_.size());
    for (const domain::SearchHit& hit : results_)
        resultLines_.push_back(formatResultLine(hit));
    selected_ = results_.empty() ? 0 : std::min(selected_, static_cast<int>(results_.size()) - 1);
    statusMessage_.clear();
}

void ResultView::append(domain::SearchHit hit)
{
    resultLines_.push_back(formatResultLine(hit));
    results_.push_back(std::move(hit));
}

std::string ResultView::formatResultLine(const domain::SearchHit& hit) const
{
    const domain::Torrent& t = hit.torrent;
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%-50.50s  %6s  %5d seeds", t.name.c_str(), humanSize(t.size).c_str(), t.seeders);
    std::string line = buf;
    if (hit.remote)
        line += "  [peer]";
    return line;
}

Element ResultView::renderDetails() const
{
    if (results_.empty() || selected_ < 0 || selected_ >= static_cast<int>(results_.size()))
        return text("No results") | dim;

    const domain::SearchHit& hit = results_[static_cast<size_t>(selected_)];
    const domain::Torrent& t = hit.torrent;

    Elements lines;
    lines.push_back(text(t.name) | bold);
    lines.push_back(text(t.hash) | dim);
    lines.push_back(separatorEmpty());
    lines.push_back(hbox({ text("size: " + humanSize(t.size) + "   "), text("files: " + std::to_string(t.files)) }));
    lines.push_back(hbox({ text("seeders: " + std::to_string(t.seeders) + "   "),
        text("leechers: " + std::to_string(t.leechers) + "   "), text("completed: " + std::to_string(t.completed)) }));
    lines.push_back(text("added: " + humanDate(t.added)));

    std::string typeCat = domain::toString(t.contentType);
    const std::string cat = domain::toString(t.contentCategory);
    if (!cat.empty())
        typeCat = typeCat.empty() ? cat : typeCat + " / " + cat;
    if (!typeCat.empty())
        lines.push_back(text("type: " + typeCat));

    lines.push_back(text("votes: +" + std::to_string(t.good) + " / -" + std::to_string(t.bad)));

    if (hit.fromFileMatch && !hit.matchingPaths.empty()) {
        lines.push_back(separatorEmpty());
        lines.push_back(text("matching files:") | dim);
        for (const std::string& p : hit.matchingPaths)
            lines.push_back(text("  " + p));
    } else if (!t.fileList.empty()) {
        lines.push_back(separatorEmpty());
        lines.push_back(text("files (" + std::to_string(t.fileList.size()) + "):") | dim);
        constexpr size_t kMaxShown = 30;
        size_t shown = 0;
        for (const domain::File& f : t.fileList) {
            if (shown >= kMaxShown) {
                lines.push_back(text("  ... and " + std::to_string(t.fileList.size() - kMaxShown) + " more") | dim);
                break;
            }
            lines.push_back(text("  " + f.path + "  (" + humanSize(f.size) + ")"));
            ++shown;
        }
    }

    return vbox(std::move(lines)) | frame;
}

Component ResultView::menu()
{
    MenuOption option;
    option.entries = &resultLines_;
    option.selected = &selected_;
    return Menu(option);
}

Element ResultView::renderPane(const ftxui::Component& menuComponent) const
{
    // statusMessage_ ('m'/'t' feedback) is a standalone status line, not part
    // of renderDetails(): it reports the outcome of an action, which stays
    // true no matter what's currently selected -- folding it into the
    // per-torrent details block (as an earlier version did) made it look
    // like info about whatever torrent the user had since scrolled to.
    Elements rightColumn;
    rightColumn.push_back(renderDetails() | flex);
    if (!statusMessage_.empty()) {
        rightColumn.push_back(separator());
        rightColumn.push_back(text(statusMessage_) | color(Color::Yellow));
    }

    return hbox({
        // yframe, not frame: frame scrolls BOTH axes to center the selected
        // entry's full (untruncated) box in the viewport, and every result
        // line here is one Menu entry whose own rendered width is wider than
        // this pane -- so the shared x-scroll it applies shifts the WHOLE
        // list's visible window right by however much the selection
        // overflows, clipping every row's start instead of its end (FTXUI
        // src/ftxui/dom/frame.cpp's Frame::SetBox). We already truncate each
        // line ourselves (formatResultLine) and only need the list to scroll
        // vertically to keep the selection in view, so drop the x-axis: a
        // plain size() box left-anchors and clips overflow on the right
        // instead, which is what a truncated-title list is supposed to look
        // like.
        menuComponent->Render() | yframe | size(WIDTH, LESS_THAN, 62),
        separator(),
        vbox(std::move(rightColumn)) | flex,
    });
}

bool ResultView::handleKey(ftxui::Event event)
{
    if (results_.empty() || selected_ < 0 || selected_ >= static_cast<int>(results_.size()))
        return false;

    if (event == Event::Character('m')) {
        statusMessage_ = "magnet: " + results_[static_cast<size_t>(selected_)].torrent.magnetLink();
        return true;
    }
    if (event == Event::Character('t')) {
        handleSaveTorrent();
        return true;
    }
    if (event == Event::Character('d')) {
        handleDownload();
        return true;
    }
    return false;
}

void ResultView::handleDownload()
{
    const domain::Torrent info = results_[static_cast<size_t>(selected_)].torrent;

    if (!downloads_) {
        statusMessage_ = "download failed: bittorrent not available (spider/mesh disabled)";
        return;
    }

    statusMessage_ = "starting download: " + (info.name.empty() ? info.hash : info.name);
    platform::log() << "ResultView: download requested for " << info.hash.substr(0, 8) << "\n";

    // addWithInfo mutates DownloadManager's registry, which is confined to
    // the EngineLoop thread (docs/M6-PLAN.md item 2) -- posted there, same
    // as every other engine call this view makes (see handleSaveTorrent).
    engine::DownloadManager* downloads = downloads_;
    engineLoop_.post([this, downloads, info] {
        const bool ok = downloads->addWithInfo(info);
        const std::string hash = info.hash;
        screen_.Post([this, ok, hash] {
            statusMessage_
                = ok ? ("downloading: " + hash.substr(0, 8) + "...") : ("already downloading: " + hash.substr(0, 8) + "...");
        });
    });
}

void ResultView::handleSaveTorrent()
{
    const std::string hash = results_[static_cast<size_t>(selected_)].torrent.hash;
    const std::filesystem::path cacheFile = std::filesystem::path(dataDir_) / "torrents" / (hash + ".torrent");

    // A cached copy from a previous save/fetch is reported immediately
    // (TorrentExporter's cache behavior, src/services/torrent_exporter.cpp) --
    // a plain stat(), cheap enough to do straight on the UI thread.
    std::error_code existsEc;
    if (std::filesystem::exists(cacheFile, existsEc)) {
        statusMessage_ = "saved: " + cacheFile.string();
        return;
    }

    if (inFlightSaves_.count(hash)) {
        statusMessage_ = "already fetching " + hash.substr(0, 8) + "...";
        return;
    }
    if (!nodeHost_ || !nodeHost_->bittorrent()) {
        statusMessage_ = "fetch failed: bittorrent not available (spider/mesh disabled)";
        return;
    }

    inFlightSaves_.insert(hash);
    statusMessage_ = "fetching metadata for " + hash.substr(0, 8) + "...";
    platform::log() << "ResultView: save-torrent " << hash.substr(0, 8)
                     << ": requesting metadata (DHT search, no known peer)\n";

    librats::Bittorrent* bittorrent = nodeHost_->bittorrent();
    engineLoop_.post([this, bittorrent, hash, cacheFile] {
        engine::fetchTorrentFile(*bittorrent, hash, [this, hash, cacheFile](std::vector<uint8_t> bytes, std::string error) {
            // get_torrent_metadata's callback fires on a librats worker
            // thread (same contract Crawler::fetchMetadata relies on) --
            // marshal back onto the EngineLoop thread for the cache-file
            // write, then post just the final status line to the UI thread.
            engineLoop_.post([this, hash, cacheFile, bytes = std::move(bytes), error = std::move(error)] {
                std::string result;
                if (bytes.empty()) {
                    result = "fetch failed: " + (error.empty() ? "unknown error" : error);
                } else {
                    std::error_code ec;
                    std::filesystem::create_directories(cacheFile.parent_path(), ec);
                    std::ofstream out(cacheFile, std::ios::binary | std::ios::trunc);
                    if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
                        result = "fetch failed: cannot write " + cacheFile.string();
                    else
                        result = "saved: " + cacheFile.string();
                }
                // Logged (not just shown in the TUI's transient status line)
                // so the outcome is still inspectable in ratsn-engine.log
                // after the details pane has moved on to something else --
                // set RATSN_BT_DEBUG=1 before launching for librats' own
                // DHT/peer-connect DEBUG lines alongside this.
                platform::log() << "ResultView: save-torrent " << hash.substr(0, 8) << ": " << result << "\n";
                screen_.Post([this, hash, result] {
                    inFlightSaves_.erase(hash);
                    statusMessage_ = result;
                });
            });
        });
    });
}

} // namespace ratsn::tui
