#pragma once

#include "domain/torrent.h"
#include "index/search_index.h"
#include "platform/engine_loop.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// The Search tab (docs/DESIGN-native.md §7): a debounced search-as-you-type
// box, a files/names toggle, a sort selector, a selectable results list and
// a details pane for the current selection.
namespace ratsn::tui {

class SearchTab {
public:
    // index is confined to the EngineLoop thread (§3); engineLoop/screen are
    // the UI<->engine bridge. All borrowed, must outlive this object.
    SearchTab(platform::EngineLoop& engineLoop, index::SearchIndex& index, ftxui::ScreenInteractive& screen);

    // Builds (once) and returns the tab's root component.
    ftxui::Component component();

    // '/' (app-level) jumps here and focuses the input; 'q' (app-level) must
    // not quit while the user is typing a literal 'q' into the query.
    void focusInput();
    bool inputFocused() const;

private:
    void triggerSearch();
    // Runs on the UI thread (either directly for an empty query, or via a
    // ScreenInteractive::Post from the debounced engine-thread query).
    // `generation` no longer matching the live counter means a newer
    // keystroke superseded this result; it's dropped instead of applied.
    void applyResults(uint64_t generation, std::vector<domain::SearchHit> hits);
    ftxui::Element renderDetails() const;
    std::string formatResultLine(const domain::SearchHit& hit) const;

    platform::EngineLoop& engineLoop_;
    index::SearchIndex& index_;
    ftxui::ScreenInteractive& screen_;

    std::string queryText_;
    bool searchFiles_ = false;
    int sortIndex_ = 0;
    std::atomic<uint64_t> generation_ { 0 };

    std::vector<domain::SearchHit> results_;
    std::vector<std::string> resultLines_; // one preformatted line per result, for Menu
    int selected_ = 0;
    std::string magnetMessage_; // set by 'm', shown in the details pane

    ftxui::Component inputComponent_;
    ftxui::Component root_;
};

} // namespace ratsn::tui
