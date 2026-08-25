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
    // for the 't' save-.torrent flow (item 6). All borrowed pointers must
    // outlive this object.
    TopTab(platform::EngineLoop& engineLoop, index::SearchIndex& index, ftxui::ScreenInteractive& screen,
        engine::NodeHost* nodeHost, std::string dataDir);

    ftxui::Component component();

    // Called by app.cpp when this tab becomes active, so the list reflects
    // the index without a background poll (docs/M5-PLAN.md: "no polling").
    void onActivated();

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
