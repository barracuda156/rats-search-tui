#pragma once

#include "domain/torrent.h"
#include "index/search_index.h"
#include "platform/config.h"
#include "platform/engine_loop.h"
#include "tui/result_view.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ratsn::engine {
class PeerApi;
class NodeHost;
class DownloadManager;
class TrackerService;
class Voting;
}

// The Search tab (docs/DESIGN-native.md §7): a debounced search-as-you-type
// box, a files/names toggle, a sort selector, a collapsible filter row
// (docs/M5-PLAN.md item 2) and a shared ResultView (item 5) for the
// selectable results list and details pane.
namespace ratsn::tui {

class SearchTab {
public:
    // index is confined to the EngineLoop thread (§3); engineLoop/screen are
    // the UI<->engine bridge. peerApi is borrowed and nullable (null when
    // the spider/mesh is disabled): when present, every search also fans out
    // to connected peers (docs/M4-PLAN.md "Remote search merge"). nodeHost is
    // borrowed and nullable, forwarded to ResultView for the 't' save-.torrent
    // flow (item 6). downloads is borrowed and nullable, forwarded to
    // ResultView for the 'd' download flow (docs/M6-PLAN.md item 5). cfg
    // supplies the strict/safe-search defaults (item 1); dataDir is
    // ResultView's .torrent cache directory. All borrowed pointers must
    // outlive this object.
    SearchTab(platform::EngineLoop& engineLoop, index::SearchIndex& index, ftxui::ScreenInteractive& screen,
        engine::PeerApi* peerApi, engine::NodeHost* nodeHost, engine::DownloadManager* downloads,
        engine::TrackerService* trackerService, engine::Voting* voting, const platform::Config& cfg,
        std::string dataDir);

    // Builds (once) and returns the tab's root component.
    ftxui::Component component();

    // '/' (app-level) jumps here and focuses the input; 'q' (app-level) must
    // not quit while the user is typing into ANY of this tab's text inputs.
    void focusInput();
    bool inputFocused() const;
    bool anyInputFocused() const;

    // Forwarded to resultView_ (docs/M8-PLAN.md item 7); see ResultView's own
    // doc comment.
    void updateSelectedStats(const std::string& hash, int seeders, int leechers, int completed, int64_t trackersCheckedMs)
    {
        resultView_.updateSelectedStats(hash, seeders, leechers, completed, trackersCheckedMs);
    }
    void updateSelectedInfo(const std::string& hash, const librats::Json& info)
    {
        resultView_.updateSelectedInfo(hash, info);
    }

private:
    void triggerSearch();
    // Runs on the UI thread (either directly for an empty query, or via a
    // ScreenInteractive::Post from the debounced engine-thread query).
    // `generation` no longer matching the live counter means a newer
    // keystroke superseded this result; it's dropped instead of applied.
    void applyResults(uint64_t generation, std::vector<domain::SearchHit> hits);
    // PeerApi's result callback (EngineLoop thread). Filtering happens here,
    // before posting to the UI thread -- see the .cpp.
    void onRemoteHit(domain::SearchHit hit);
    // EngineLoop-thread only, consulted from onRemoteHit against
    // remoteFilters_ (this tab's query/filter snapshot as of the last
    // broadcast -- see remoteFilters_'s own comment).
    bool passesStrictTokenCheck(const domain::SearchHit& hit) const;
    bool passesLocalFilters(const domain::SearchHit& hit) const;

    platform::EngineLoop& engineLoop_;
    index::SearchIndex& index_;
    ftxui::ScreenInteractive& screen_;
    engine::PeerApi* peerApi_;

    ResultView resultView_;

    std::string queryText_;
    bool searchFiles_ = false;
    int sortIndex_ = 0;
    std::atomic<uint64_t> generation_ { 0 };
    // The full query active when the last remote broadcast was sent (text +
    // every filter field); arriving hits are matched against it in
    // onRemoteHit -- size/files/seeders/type/tracker/strict can't ride the
    // wire (docs/M5-PLAN.md items 1/2), so this is how remote hits get the
    // same filtering local hits get from Groonga. EngineLoop-thread only,
    // like remoteSearchGeneration_ below.
    index::SearchQuery remoteFilters_;
    uint64_t remoteSearchGeneration_ = 0;

    // --- Filter row state (docs/M5-PLAN.md item 2), hidden by default,
    // toggled with 'f' ---
    bool filtersVisible_ = false;
    bool strict_ = true;
    bool safe_ = false;
    int typeIndex_ = 0;
    std::string sizeMinText_;
    std::string sizeMaxText_;
    std::string seedersMinText_;
    std::string trackerText_;

    ftxui::Component inputComponent_;
    ftxui::Component sizeMinInput_;
    ftxui::Component sizeMaxInput_;
    ftxui::Component seedersMinInput_;
    ftxui::Component trackerInput_;
    ftxui::Component root_;
};

} // namespace ratsn::tui
