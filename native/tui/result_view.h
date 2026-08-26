#pragma once

#include "domain/torrent.h"
#include "platform/engine_loop.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <string>
#include <unordered_set>
#include <vector>

namespace ratsn::engine {
class NodeHost;
class DownloadManager;
class TrackerService;
class Voting;
}

// Result-list + details-pane rendering shared by the Search and Top tabs
// (docs/DESIGN-native.md §7; mechanical extraction out of search_tab.cpp,
// docs/M5-PLAN.md item 5 -- no behavior change to the Search tab). Owns the
// selectable results list, its details pane, and the 'm' (show magnet) / 't'
// (save .torrent, item 6) key handling; the owning tab is responsible for
// populating results and for its own extra keys/focus guards.
namespace ratsn::tui {

class ResultView {
public:
    // engineLoop/screen are the UI<->engine bridge. nodeHost is borrowed and
    // nullable (null when the spider/mesh is disabled -- 't' then always
    // reports "not available", same as TorrentExporter's isReady() check in
    // the Qt app). dataDir is where "<dataDir>/torrents/<hash>.torrent" is
    // cached/read (mirrors TorrentExporter's cache layout). downloads is
    // borrowed and nullable (null when the BitTorrent client never came up
    // -- 'd' then always reports "not available", docs/M6-PLAN.md item 5).
    // trackerService is borrowed (docs/M8-PLAN.md item 7); non-null in
    // practice (the scrapers need no NodeHost/BitTorrent client and are
    // always constructed) but treated as nullable here for the same
    // defensive reason as the pointers above. voting is borrowed
    // (docs/M7-PLAN.md item 7); non-null in practice (Voting is always
    // constructed, degrading to local-only counts when storage is
    // unavailable) but treated as nullable here for the same defensive
    // reason.
    ResultView(platform::EngineLoop& engineLoop, ftxui::ScreenInteractive& screen, engine::NodeHost* nodeHost,
        engine::DownloadManager* downloads, engine::TrackerService* trackerService, engine::Voting* voting,
        std::string dataDir);

    // Replaces the whole result set (a fresh search/top query). UI-thread
    // only (see the .cpp -- mirrors the old SearchTab::applyResults).
    void setResults(std::vector<domain::SearchHit> hits);
    // Incremental add for remote hits (SearchTab only); the caller already
    // deduped via hasHash() and applied its own filters. UI-thread only.
    void append(domain::SearchHit hit);
    bool hasHash(const std::string& hash) const;
    bool empty() const { return results_.empty(); }

    // Builds (once) and returns the results Menu component, bound to this
    // view's selection state.
    ftxui::Component menu();
    // Combines `menuComponent`'s render with the details pane and (below it,
    // as its own line) the statusMessage_ status line.
    ftxui::Element renderPane(const ftxui::Component& menuComponent) const;

    // 'm'/'t'/'d'/'v'/'V' handling (docs/M5-PLAN.md items 5/6, docs/M6-PLAN.md
    // item 5, docs/M7-PLAN.md item 7); returns true if the event was
    // consumed. The caller's own CatchEvent should apply its focus guard
    // first, then delegate here.
    bool handleKey(ftxui::Event event);

    // In-place refresh when a tracker scrape completes for a hash currently
    // held in this view's result set (docs/M8-PLAN.md item 7/deviation #4 --
    // the native stand-in for Qt's torrentUpdated signal + full panel
    // reload). UI-thread only; the caller (app.cpp) marshals via screen.Post.
    // No-op if the hash isn't in the current result set (e.g. the view has
    // since reloaded).
    void updateSelectedStats(const std::string& hash, int seeders, int leechers, int completed, int64_t trackersCheckedMs);
    void updateSelectedInfo(const std::string& hash, const librats::Json& info);

private:
    ftxui::Element renderDetails() const;
    std::string formatResultLine(const domain::SearchHit& hit) const;
    void handleSaveTorrent();
    void handleDownload();
    void handleVote(bool good);
    void updateSelectedVotes(const std::string& hash, int good, int bad);
    // Menu's on_change hook (docs/M8-PLAN.md item 7): posts checkCounts always,
    // checkInfo only when the selected torrent has no tracker identity yet.
    void onSelectionChanged();

    platform::EngineLoop& engineLoop_;
    ftxui::ScreenInteractive& screen_;
    engine::NodeHost* nodeHost_;
    engine::DownloadManager* downloads_;
    engine::TrackerService* trackerService_;
    engine::Voting* voting_;
    std::string dataDir_;

    std::vector<domain::SearchHit> results_;
    std::vector<std::string> resultLines_; // one preformatted line per result, for Menu
    int selected_ = 0;
    // Set by 'm'/'t'; rendered as its own line below the details pane (see
    // renderPane), not inside it -- it reports an action's outcome, which
    // stays true regardless of what's currently selected, so it must not
    // read as info about whichever torrent the selection has since moved to.
    std::string statusMessage_;

    // Hashes with a save-.torrent fetch in flight (item 6); UI-thread only
    // (touched only from handleSaveTorrent and the screen_.Post completion
    // below).
    std::unordered_set<std::string> inFlightSaves_;
};

} // namespace ratsn::tui
