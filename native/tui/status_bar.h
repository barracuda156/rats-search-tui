#pragma once

#include "engine/crawler.h"
#include "engine/downloads.h"
#include "engine/node_host.h"
#include "index/search_index.h"
#include "platform/engine_loop.h"

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <chrono>
#include <cstddef>
#include <string>

// The bottom status bar (visible under every tab) and the dedicated Status
// tab's fuller view, per docs/DESIGN-native.md §7. Bundled in one file since
// both render from the same live snapshot and the module breakdown (§4)
// doesn't list a separate status-tab file.
namespace ratsn::tui {

// Everything the bottom status bar and the Status tab render from. Mutated
// only inside ScreenInteractive::Post callbacks (the engine->UI channel,
// §3), so plain fields are safe here -- Loop() and Post() both run on the
// same UI thread, never concurrently with each other.
struct StatusModel {
    index::IndexStats indexStats;
    bool dhtRunning = false;
    size_t dhtNodes = 0;
    size_t spiderPool = 0;
    size_t spiderVisited = 0;
    int discovered = 0;
    int activeFetches = 0;

    // Downloads (M6): active-download count + aggregate speed, refreshed by
    // StatusUpdater like everything else above.
    int dlActive = 0;
    double dlSpeed = 0.0;

    // A transient "download completed" flash (docs/M6-PLAN.md item 2's
    // "optional completion callback for the status-bar flash"), set by
    // app.cpp's DownloadManager::setCompletionCallback registration.
    // downloadFlashUntil is a deadline rather than a tick countdown so
    // renderStatusBar (called on every UI redraw) can expire it itself --
    // StatusModel is otherwise only ever touched inside a
    // ScreenInteractive::Post callback (see the struct comment above), and a
    // countdown decremented from StatusUpdater's engine-thread tick() would
    // violate that.
    std::string downloadFlash;
    std::chrono::steady_clock::time_point downloadFlashUntil {};
};

// Values that don't change over the run, unlike StatusModel.
struct StatusInfo {
    std::string dataDir;
    std::string nodeId;
    int listenPort = 0;
    bool spiderEnabled = false;
    bool fileIndexEnabled = false;
};

// Self-rescheduling ~1Hz ticker (§7's "coalesce index-count ticks to 1/s;
// never post per-torrent" rule): reads index/nodeHost/crawler on the
// EngineLoop thread, then hands a snapshot to the UI thread via
// ScreenInteractive::Post. Mirrors main.cpp's StatsPrinter but feeds a UI
// model instead of stdout. Stops rescheduling once the engine loop is
// stopped; closing the UI itself in that case is the caller's job (a
// separate watcher thread, since relying on this ticker's own queued
// reschedule to notice the stop is racy -- see native/tui/app.cpp).
class StatusUpdater {
public:
    StatusUpdater(platform::EngineLoop& loop, index::SearchIndex& index, engine::NodeHost* nodeHost,
        engine::Crawler* crawler, engine::DownloadManager* downloads, ftxui::ScreenInteractive& screen,
        StatusModel& model)
        : loop_(loop)
        , index_(index)
        , nodeHost_(nodeHost)
        , crawler_(crawler)
        , downloads_(downloads)
        , screen_(screen)
        , model_(model)
    {
    }

    void start(int intervalMs = 1000)
    {
        intervalMs_ = intervalMs;
        schedule();
    }

private:
    void schedule() { loop_.postDelayed([this] { tick(); }, intervalMs_); }

    void tick()
    {
        if (!loop_.isRunning())
            return;

        StatusModel snapshot;
        snapshot.indexStats = index_.counts();
        if (nodeHost_) {
            snapshot.dhtRunning = nodeHost_->isDhtRunning();
            snapshot.dhtNodes = nodeHost_->dhtNodeCount();
            snapshot.spiderPool = nodeHost_->spiderPoolSize();
            snapshot.spiderVisited = nodeHost_->spiderVisitedCount();
        }
        if (crawler_) {
            snapshot.discovered = crawler_->discoveredCount();
            snapshot.activeFetches = crawler_->activeFetches();
        }
        if (downloads_) {
            const auto agg = downloads_->aggregate();
            snapshot.dlActive = agg.active;
            snapshot.dlSpeed = agg.downloadSpeed;
        }
        screen_.Post([this, snapshot] {
            // snapshot carries none of the completion flash's state (it's
            // set independently by DownloadManager::setCompletionCallback's
            // own screen_.Post, between ticks) -- preserve it across this
            // wholesale replace instead of dropping whatever flash is live.
            const std::string flash = model_.downloadFlash;
            const auto flashUntil = model_.downloadFlashUntil;
            model_ = snapshot;
            model_.downloadFlash = flash;
            model_.downloadFlashUntil = flashUntil;
        });

        schedule();
    }

    platform::EngineLoop& loop_;
    index::SearchIndex& index_;
    engine::NodeHost* nodeHost_;
    engine::Crawler* crawler_;
    engine::DownloadManager* downloads_;
    ftxui::ScreenInteractive& screen_;
    StatusModel& model_;
    int intervalMs_ = 1000;
};

ftxui::Element renderStatusBar(const StatusModel& model);
ftxui::Element renderStatusTab(const StatusModel& model, const StatusInfo& info);

} // namespace ratsn::tui
