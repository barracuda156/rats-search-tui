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
    ResultView(platform::EngineLoop& engineLoop, ftxui::ScreenInteractive& screen, engine::NodeHost* nodeHost,
        engine::DownloadManager* downloads, std::string dataDir);

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

    // 'm'/'t'/'d' handling (docs/M5-PLAN.md items 5/6, docs/M6-PLAN.md item
    // 5); returns true if the event was consumed. The caller's own
    // CatchEvent should apply its focus guard first, then delegate here.
    bool handleKey(ftxui::Event event);

private:
    ftxui::Element renderDetails() const;
    std::string formatResultLine(const domain::SearchHit& hit) const;
    void handleSaveTorrent();
    void handleDownload();

    platform::EngineLoop& engineLoop_;
    ftxui::ScreenInteractive& screen_;
    engine::NodeHost* nodeHost_;
    engine::DownloadManager* downloads_;
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
