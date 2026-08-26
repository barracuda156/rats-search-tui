#include "tui/app.h"

#include "tui/downloads_tab.h"
#include "tui/search_tab.h"
#include "tui/top_tab.h"

#include "platform/log.h"

#include "librats/util/logger.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

using namespace ftxui;

namespace ratsn::tui {

namespace {

Element renderTabBar(int tabIndex)
{
    auto label = [&](int idx, const std::string& name) {
        Element e = text(" " + name + " ");
        return idx == tabIndex ? e | inverted | bold : e | dim;
    };
    return hbox({
        label(0, "Search"),
        label(1, "Top"),
        label(2, "Downloads"),
        label(3, "Status"),
        filler(),
        text(" Tab: switch tab   /: search   d: download   q: quit ") | dim,
    });
}

} // namespace

void run(platform::EngineLoop& engineLoop, std::thread& engineThread, index::SearchIndex& index,
    engine::NodeHost* nodeHost, engine::Crawler* crawler, engine::PeerApi* peerApi, engine::DownloadManager* downloads,
    engine::TrackerService* trackerService, const platform::Config& cfg, const StatusInfo& info)
{
    // ScreenInteractive::Fullscreen() owns the terminal exclusively via the
    // alternate screen buffer and repaints it wholesale on every redraw.
    // librats' own Logger writes its DHT/network diagnostics straight to
    // std::cout/std::cerr by default (console-enabled by default, see
    // src/librats/util/logger.h) -- those lines land wherever the cursor
    // happens to be and flash on screen until the next FTXUI redraw wipes
    // them. Fixed by using the Logger's own file-logging mode instead of
    // redirecting std::cout/std::cerr globally: FTXUI's own screen writes
    // ALSO go through std::cout (App::Internal::TerminalFlush, in FTXUI's
    // own source) -- a blanket stream redirect silently swallows the TUI's
    // rendering right along with the log spam, which is exactly what
    // happened the first time this was "fixed" (nothing but the pre-launch
    // startup log ever reached the terminal). Redirecting the Logger
    // specifically has no such conflict: it's a separate file handle, not a
    // shared global stream. Settings are captured and restored so a future
    // caller of Logger::getInstance() (e.g. --console) isn't left pointed at
    // this session's log file.
    librats::Logger& logger = librats::Logger::getInstance();
    const bool priorConsoleLogging = logger.is_console_logging_enabled();
    const bool priorFileLogging = logger.is_file_logging_enabled();
    const std::string priorLogFilePath = logger.get_log_file_path();
    const librats::LogLevel priorLogLevel = logger.get_log_level();
    logger.set_log_file_path((std::filesystem::path(info.dataDir) / "ratsn.log").string());
    logger.set_file_logging_enabled(true);
    logger.set_console_logging_enabled(false);
    // Default level is INFO, which filters out librats' own DHT get_peers/
    // announce/peer-connect lines (all LOG_DEBUG) -- exactly the lines that
    // would show whether the 't' save-.torrent flow's DHT-only metadata
    // search (see torrent_file.h) ever finds a peer at all. Opt-in only:
    // DEBUG is chatty and this log already fills fast under the mesh (M4).
    if (std::getenv("RATSN_BT_DEBUG"))
        logger.set_log_level(librats::LogLevel::DEBUG);

    // Same problem, second source: native/engine/{crawler,indexer,peer_api,
    // peer_registry,replication}.cpp's own diagnostic prints (kept
    // deliberately for --console) go through platform::log() rather than
    // std::cout/std::cerr directly, for exactly the reason above -- and
    // since M4 (the peer mesh) that sink fires on close to every wire
    // message from every connected peer, which is what actually flashes the
    // screen badly enough to be worth fixing (event-driven crawl/index
    // diagnostics alone were rare enough to leave alone through M3).
    // Redirected to its own file here, same idea as the Logger above: a
    // separate handle, not a shared stream, so it can't ever swallow
    // FTXUI's own std::cout writes.
    platform::enableFileLogging((std::filesystem::path(info.dataDir) / "ratsn-engine.log").string());

    ScreenInteractive screen = ScreenInteractive::Fullscreen();

    StatusModel statusModel;
    StatusUpdater statusUpdater(engineLoop, index, nodeHost, crawler, downloads, screen, statusModel);

    // A download completing flashes the status bar (docs/M6-PLAN.md item 2's
    // "optional completion callback"); fires on the EngineLoop thread, so
    // marshal before touching statusModel (same idiom as every other
    // engine->UI callback in this file).
    if (downloads) {
        downloads->setCompletionCallback([&screen, &statusModel](const std::string&, const std::string& name) {
            screen.Post([&statusModel, name] {
                statusModel.downloadFlash = "download completed: " + name;
                statusModel.downloadFlashUntil = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            });
        });
    }

    SearchTab searchTab(engineLoop, index, screen, peerApi, nodeHost, downloads, trackerService, cfg, info.dataDir);
    Component searchComponent = searchTab.component();
    TopTab topTab(engineLoop, index, screen, nodeHost, downloads, trackerService, info.dataDir);
    Component topComponent = topTab.component();

    // A tracker scrape completing flashes the matching row live in whichever
    // tab currently holds it (docs/M8-PLAN.md item 7/deviation #4) -- fires
    // on the engine thread, so marshal before touching either tab (same
    // idiom as the download-completion callback below).
    if (trackerService) {
        trackerService->setStatsUpdatedCallback(
            [&screen, &searchTab, &topTab](const std::string& hash, int seeders, int leechers, int completed, int64_t trackersCheckedMs) {
                screen.Post([&searchTab, &topTab, hash, seeders, leechers, completed, trackersCheckedMs] {
                    searchTab.updateSelectedStats(hash, seeders, leechers, completed, trackersCheckedMs);
                    topTab.updateSelectedStats(hash, seeders, leechers, completed, trackersCheckedMs);
                });
            });
        trackerService->setInfoUpdatedCallback([&screen, &searchTab, &topTab](const std::string& hash, const librats::Json& info) {
            screen.Post([&searchTab, &topTab, hash, info] {
                searchTab.updateSelectedInfo(hash, info);
                topTab.updateSelectedInfo(hash, info);
            });
        });
    }
    DownloadsTab downloadsTab(engineLoop, screen, downloads);
    Component downloadsComponent = downloadsTab.component();
    Component statusComponent = Renderer([&statusModel, &info] { return renderStatusTab(statusModel, info); });

    int tabIndex = 0;
    int priorTabIndex = 0;
    Component tabContent
        = Container::Tab({ searchComponent, topComponent, downloadsComponent, statusComponent }, &tabIndex);
    // Reload the Top tab's list on activation (docs/M5-PLAN.md item 5: "no
    // polling") -- Container::Tab has no activation callback of its own, so
    // this is checked once per frame in the top-level Renderer below instead.
    // The Downloads tab needs no such hook: it polls on its own 1s timer
    // regardless of which tab is visible (docs/M6-PLAN.md item 5), started
    // once below alongside statusUpdater.

    constexpr int kTabCount = 4;
    Component layout = Renderer(tabContent, [&] {
        if (tabIndex != priorTabIndex) {
            if (tabIndex == 1)
                topTab.onActivated();
            priorTabIndex = tabIndex;
        }
        return vbox({
            renderTabBar(tabIndex),
            separator(),
            tabContent->Render() | flex,
            separator(),
            renderStatusBar(statusModel),
        });
    });

    // Wrapping the whole tree means these are seen before any inner
    // component (Container's own Tab-key focus-cycling, Menu's keys, ...)
    // -- CatchEvent is capture-first, not bubble-up (ftxui/src/component/
    // catch_event.cpp: on_event_ runs before delegating to the child).
    // That's why 'q' explicitly checks anyInputFocused() -- otherwise it
    // would steal a literal 'q' keystroke out of a search/filter box.
    layout = CatchEvent(layout, [&](Event event) {
        if (event == Event::Tab) {
            tabIndex = (tabIndex + 1) % kTabCount;
            return true;
        }
        if (event == Event::TabReverse) {
            tabIndex = (tabIndex + kTabCount - 1) % kTabCount;
            return true;
        }
        if (event == Event::Character('/')) {
            tabIndex = 0;
            searchTab.focusInput();
            return true;
        }
        if (event == Event::Character('q') && !searchTab.anyInputFocused() && !downloadsTab.inputFocused()) {
            screen.ExitLoopClosure()();
            return true;
        }
        return false;
    });

    // Closes the UI if the engine loop is stopped externally (SIGINT/
    // SIGTERM, see main.cpp's handleSigint) while the user hasn't pressed
    // 'q'. Deliberately independent of the EngineLoop's own task queue: a
    // 1Hz ticker noticing its own stop (see StatusUpdater::tick) is racy --
    // the very last tick can be scheduled just before EngineLoop::run()'s
    // loop observes stopRequested_ and exits, in which case that reschedule
    // never fires and nothing would ever close the UI. Polling the atomic
    // isRunning() flag directly from an independent thread has no such gap.
    std::atomic<bool> uiActive { true };
    std::thread watcher([&] {
        // engineThread was just started by the caller and isRunning() only
        // flips true once EngineLoop::run() actually begins on it -- wait
        // that out first, or a slow-to-schedule engineThread reads as
        // "already stopped" and closes the UI before it ever opened.
        while (!engineLoop.isRunning() && uiActive.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        while (engineLoop.isRunning() && uiActive.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (uiActive.load(std::memory_order_relaxed))
            screen.Post([&screen] { screen.Exit(); });
    });

    statusUpdater.start();
    downloadsTab.start();
    screen.Loop(layout);

    uiActive.store(false, std::memory_order_relaxed);
    watcher.join();

    // Stop and join the engine thread here, while `screen` (and searchTab/
    // statusUpdater, which hold a reference to it) are still alive: any
    // task already dequeued by the engine thread when stop() is observed
    // still runs to completion (EngineLoop::run() finishes its current
    // batch before rechecking stopRequested_), and some of those tasks call
    // screen.Post(...). Joining before this function returns -- rather than
    // leaving it to the caller, which owns engineThread but not screen --
    // guarantees no such call can ever touch an already-destroyed screen.
    engineLoop.stop();
    if (engineThread.joinable())
        engineThread.join();

    // Restored last, after the engine thread is fully joined -- a task that
    // fired right at shutdown may still have logged something, and that
    // should land in the file too, not the terminal mid-teardown of the
    // alternate screen. Same reasoning covers platform::log(): every engine
    // component above writes through it, so it must stay file-redirected
    // until nothing that could call it (the just-joined engine thread) is
    // still running.
    logger.set_console_logging_enabled(priorConsoleLogging);
    logger.set_file_logging_enabled(priorFileLogging);
    logger.set_log_file_path(priorLogFilePath);
    logger.set_log_level(priorLogLevel);
    platform::disableFileLogging();
}

} // namespace ratsn::tui
