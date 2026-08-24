#include "tui/app.h"

#include "tui/search_tab.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <chrono>
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
        label(1, "Status"),
        filler(),
        text(" Tab: switch tab   /: search   q: quit ") | dim,
    });
}

} // namespace

void run(platform::EngineLoop& engineLoop, std::thread& engineThread, index::SearchIndex& index,
    engine::NodeHost* nodeHost, engine::Crawler* crawler, const StatusInfo& info)
{
    ScreenInteractive screen = ScreenInteractive::Fullscreen();

    StatusModel statusModel;
    StatusUpdater statusUpdater(engineLoop, index, nodeHost, crawler, screen, statusModel);

    SearchTab searchTab(engineLoop, index, screen);
    Component searchComponent = searchTab.component();
    Component statusComponent = Renderer([&statusModel, &info] { return renderStatusTab(statusModel, info); });

    int tabIndex = 0;
    Component tabContent = Container::Tab({ searchComponent, statusComponent }, &tabIndex);

    Component layout = Renderer(tabContent, [&] {
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
    // That's why 'q' explicitly checks inputFocused() -- otherwise it would
    // steal a literal 'q' keystroke out of the search box.
    layout = CatchEvent(layout, [&](Event event) {
        if (event == Event::Tab || event == Event::TabReverse) {
            tabIndex = 1 - tabIndex;
            return true;
        }
        if (event == Event::Character('/')) {
            tabIndex = 0;
            searchTab.focusInput();
            return true;
        }
        if (event == Event::Character('q') && !searchTab.inputFocused()) {
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
}

} // namespace ratsn::tui
