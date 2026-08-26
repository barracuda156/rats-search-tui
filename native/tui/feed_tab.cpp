#include "tui/feed_tab.h"

using namespace ftxui;

namespace ratsn::tui {

FeedTab::FeedTab(platform::EngineLoop& engineLoop, engine::Feed& feed, ftxui::ScreenInteractive& screen,
    engine::NodeHost* nodeHost, engine::DownloadManager* downloads, engine::TrackerService* trackerService,
    engine::Voting* voting, std::string dataDir)
    : engineLoop_(engineLoop)
    , feed_(feed)
    , screen_(screen)
    , resultView_(engineLoop, screen, nodeHost, downloads, trackerService, voting, std::move(dataDir))
{
}

void FeedTab::onActivated()
{
    reload();
}

void FeedTab::reload()
{
    const uint64_t gen = ++generation_;

    engineLoop_.post([this, gen] {
        if (generation_.load(std::memory_order_relaxed) != gen)
            return;

        // Read revision_ before the copy below (not after): a mutation
        // landing between the two would otherwise be lost -- the next
        // needsReload() check would see the already-current revision and
        // never notice.
        const uint64_t rev = feed_.revision();
        std::vector<engine::FeedItem> items = feed_.getFeed(0, feed_.size());

        std::vector<domain::SearchHit> hits;
        hits.reserve(items.size());
        for (engine::FeedItem& item : items) {
            domain::SearchHit hit;
            hit.torrent = std::move(item.torrent);
            hit.feedDate = item.feedDate;
            hits.push_back(std::move(hit));
        }

        screen_.Post([this, gen, rev, hits = std::move(hits)]() mutable { applyResults(gen, rev, std::move(hits)); });
    });
}

void FeedTab::applyResults(uint64_t generation, uint64_t revision, std::vector<domain::SearchHit> hits)
{
    if (generation_.load(std::memory_order_relaxed) != generation)
        return;
    lastSeenRevision_ = revision;
    resultView_.setResults(std::move(hits));
}

Component FeedTab::component()
{
    Component resultsMenu = resultView_.menu();

    root_ = Renderer(resultsMenu, [this, resultsMenu] { return resultView_.renderPane(resultsMenu) | flex; });

    root_ = CatchEvent(root_, [this](Event event) { return resultView_.handleKey(event); });

    return root_;
}

} // namespace ratsn::tui
