#pragma once

#include "domain/torrent.h"
#include "index/search_index.h"
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
class NodeHost;
class DownloadManager;
class TrackerService;
}

// The Top tab (docs/M5-PLAN.md item 5): content-type + time-window toggles
// over index_.top(), rendered through the same ResultView (item 5) the
// Search tab uses. No search-as-you-type debounce needed -- it only reloads
// on tab activation and toggle changes, not per keystroke.
namespace ratsn::tui {

class TopTab {
public:
    // index is confined to the EngineLoop thread (§3); engineLoop/screen are
    // the UI<->engine bridge. nodeHost/dataDir are forwarded to ResultView
    // for the 't' save-.torrent flow (item 6); downloads for the 'd'
    // download flow (docs/M6-PLAN.md item 5). All borrowed pointers must
    // outlive this object.
    TopTab(platform::EngineLoop& engineLoop, index::SearchIndex& index, ftxui::ScreenInteractive& screen,
        engine::NodeHost* nodeHost, engine::DownloadManager* downloads, engine::TrackerService* trackerService,
        std::string dataDir);

    ftxui::Component component();

    // Called by app.cpp when this tab becomes active, so the list reflects
    // the index without a background poll (docs/M5-PLAN.md: "no polling").
    void onActivated();

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
    void reload();
    void applyResults(uint64_t generation, std::vector<domain::Torrent> torrents);

    platform::EngineLoop& engineLoop_;
    index::SearchIndex& index_;
    ftxui::ScreenInteractive& screen_;

    ResultView resultView_;

    int typeIndex_ = 0;
    int timeIndex_ = 0; // all/day/week/month -- see kTimeValues in the .cpp
    std::atomic<uint64_t> generation_ { 0 };

    ftxui::Component root_;
};

} // namespace ratsn::tui
