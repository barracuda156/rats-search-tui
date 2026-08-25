#pragma once

#include "engine/downloads.h"
#include "platform/engine_loop.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// The Downloads tab (docs/M6-PLAN.md item 5): list + details pane over
// DownloadManager's snapshot, an add-input row (magnet/hash/.torrent path)
// and pause/remove/delete keys. DownloadManager is engine-thread-confined
// (docs/M6-PLAN.md item 2), so every mutation here is posted to the
// EngineLoop, mirroring ResultView's 'd'/'t' idiom.
namespace ratsn::tui {

class DownloadsTab {
public:
    // engineLoop/screen are the UI<->engine bridge. downloads is borrowed
    // and nullable (null when the BitTorrent client never came up -- the
    // tab then shows no downloads and every action reports failure, same as
    // ResultView's 'd'/'t' when their engine pointers are null). Must
    // outlive this object.
    DownloadsTab(platform::EngineLoop& engineLoop, ftxui::ScreenInteractive& screen, engine::DownloadManager* downloads);

    ftxui::Component component();

    // Begins the 1s snapshot poll (call once, after component() -- mirrors
    // StatusUpdater::start()).
    void start();

    // 'q' (app-level) must not quit while the user is typing into the
    // add-input row.
    bool inputFocused() const;

private:
    void schedule();
    void tick(); // engine-thread; self-reschedules via EngineLoop::postDelayed
    void applySnapshot(std::vector<engine::Download> rows); // UI-thread

    void handleAddSubmit();
    bool handleKey(ftxui::Event event);

    ftxui::Element renderDetails() const;
    std::string formatRow(const engine::Download& d) const;

    platform::EngineLoop& engineLoop_;
    ftxui::ScreenInteractive& screen_;
    engine::DownloadManager* downloads_;

    std::vector<engine::Download> rows_;
    std::vector<std::string> rowLines_; // one preformatted line per row, for Menu
    int selected_ = 0;
    uint64_t lastRevision_ = 0;

    std::string addText_;
    // Set by pause/remove/delete/add; rendered as its own status line (same
    // idiom as ResultView::statusMessage_).
    std::string statusMessage_;

    // 'X' pressed twice on the same row within a few seconds deletes files
    // (docs/M6-PLAN.md deviation #3); a single 'X', a different row, or the
    // window elapsing just re-arms/disarms instead.
    std::string armedDeleteHash_;
    std::chrono::steady_clock::time_point armedAt_;

    ftxui::Component addInput_;
    ftxui::Component root_;
};

} // namespace ratsn::tui
