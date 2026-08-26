#pragma once

#include "engine/crawler.h"
#include "engine/downloads.h"
#include "engine/feed.h"
#include "engine/node_host.h"
#include "engine/peer_api.h"
#include "engine/tracker_service.h"
#include "engine/voting.h"
#include "index/search_index.h"
#include "platform/config.h"
#include "platform/engine_loop.h"
#include "tui/status_bar.h"

#include <thread>

// Top-level FTXUI application (docs/DESIGN-native.md §7): wires the tab bar,
// Search/Top/Feed/Downloads/Status tabs (docs/M5-PLAN.md item 5, docs/
// M7-PLAN.md item 7) and the bottom status bar together and drives the
// blocking main loop. See native/main.cpp's cmdTui for how the engine
// (NodeHost/Crawler/EngineLoop) is constructed and started before this runs.
namespace ratsn::tui {

// Runs the FTXUI interactive UI on the calling thread until the user quits
// ('q') or the engine loop is stopped externally (SIGINT/SIGTERM), then
// stops `engineLoop` and joins `engineThread` before returning -- the caller
// must not touch either afterward assuming they're still running, and must
// not join engineThread itself (this function already did; see app.cpp for
// why that ordering, not the caller doing it, is what keeps this safe).
// `engineLoop` must already be running on `engineThread` when this is
// called. index, nodeHost, crawler, peerApi and downloads are borrowed and
// must outlive this call; nodeHost/crawler/peerApi are null when the
// spider/mesh is disabled, downloads is null when the BitTorrent client
// never came up (see main.cpp's EnginePipeline). trackerService is borrowed
// (docs/M8-PLAN.md item 7); non-null in practice (M8's scrapers need no
// NodeHost/BitTorrent client and are always constructed). voting/feed are
// borrowed (docs/M7-PLAN.md item 7); also non-null in practice (both are
// constructed unconditionally by startEnginePipeline, degrading gracefully
// when storage/the node is unavailable) -- feed is dereferenced directly
// when building the Feed tab, same assumption `index` already gets. cfg
// supplies the Search tab's strict/safe-search defaults (docs/M5-PLAN.md
// item 1).
void run(platform::EngineLoop& engineLoop, std::thread& engineThread, index::SearchIndex& index,
    engine::NodeHost* nodeHost, engine::Crawler* crawler, engine::PeerApi* peerApi, engine::DownloadManager* downloads,
    engine::TrackerService* trackerService, engine::Voting* voting, engine::Feed* feed, const platform::Config& cfg,
    const StatusInfo& info);

} // namespace ratsn::tui
