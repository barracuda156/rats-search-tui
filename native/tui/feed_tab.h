#pragma once

#include "domain/torrent.h"
#include "engine/feed.h"
#include "platform/engine_loop.h"
#include "tui/result_view.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <cstdint>
#include <string>

namespace ratsn::engine {
class NodeHost;
class DownloadManager;
class TrackerService;
class Voting;
}

// The Feed tab (docs/M7-PLAN.md item 7): the ranked voted-torrents feed
// (engine::Feed), rendered through the same ResultView the Search/Top tabs
// use -- a straightforward clone of TopTab's shape, minus the content-type/
// time toggles (the feed has no such filters; its order is entirely the
// feed's own ranking). Reloads on tab activation (onActivated, mirroring
// TopTab) and whenever feed.revision() has moved since the last reload
// (needsReload, polled once per frame in app.cpp's top-level Renderer --
// same place the Top tab's own activation check already runs) so a vote or
// feed-sync update is picked up even while this tab is already visible.
namespace ratsn::tui {

class FeedTab {
public:
    // feed is confined to the EngineLoop thread (§3) and must outlive this
    // object, as must every other borrowed pointer (nodeHost/downloads/
    // trackerService/voting, forwarded to ResultView exactly like TopTab).
    FeedTab(platform::EngineLoop& engineLoop, engine::Feed& feed, ftxui::ScreenInteractive& screen,
        engine::NodeHost* nodeHost, engine::DownloadManager* downloads, engine::TrackerService* trackerService,
        engine::Voting* voting, std::string dataDir);

    ftxui::Component component();

    // Called by app.cpp when this tab becomes active, or when needsReload()
    // reports a pending change.
    void onActivated();
    // True once feed.revision() has moved past the value as of the last
    // reload -- cheap (atomic load), safe to call from the UI thread every
    // frame.
    bool needsReload() const { return feed_.revision() != lastSeenRevision_; }

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
    void applyResults(uint64_t generation, uint64_t revision, std::vector<domain::SearchHit> hits);

    platform::EngineLoop& engineLoop_;
    engine::Feed& feed_;
    ftxui::ScreenInteractive& screen_;

    ResultView resultView_;

    uint64_t lastSeenRevision_ = 0;
    std::atomic<uint64_t> generation_ { 0 };

    ftxui::Component root_;
};

} // namespace ratsn::tui
