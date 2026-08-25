#include "tui/top_tab.h"

#include "tui/format.h"

using namespace ftxui;

namespace ratsn::tui {

namespace {
const std::vector<std::string> kTimeLabels = { "all", "day", "week", "month" };
// groonga_index.cpp's top() treats the "hours" filter key as a 24h cutoff
// (docs/M5-PLAN.md item 5: "the 'hours' key actually means 24h ... label it
// 'day', keep the key" -- kept as-is rather than renamed, since it's already
// the wire/CLI vocabulary PeerApi and `ratsn top --time` both use).
constexpr const char* kTimeValues[] = { "", "hours", "week", "month" };
} // namespace

TopTab::TopTab(platform::EngineLoop& engineLoop, index::SearchIndex& index, ftxui::ScreenInteractive& screen,
    engine::NodeHost* nodeHost, engine::DownloadManager* downloads, std::string dataDir)
    : engineLoop_(engineLoop)
    , index_(index)
    , screen_(screen)
    , resultView_(engineLoop, screen, nodeHost, downloads, std::move(dataDir))
{
}

void TopTab::onActivated()
{
    reload();
}

void TopTab::reload()
{
    const uint64_t gen = ++generation_;

    index::TopQuery q;
    q.contentType = kContentTypeValues[static_cast<size_t>(typeIndex_)];
    q.time = kTimeValues[static_cast<size_t>(timeIndex_)];
    q.limit = 200;

    engineLoop_.post([this, gen, q] {
        if (generation_.load(std::memory_order_relaxed) != gen)
            return;
        std::vector<domain::Torrent> torrents = index_.top(q);
        screen_.Post([this, gen, torrents = std::move(torrents)]() mutable { applyResults(gen, std::move(torrents)); });
    });
}

void TopTab::applyResults(uint64_t generation, std::vector<domain::Torrent> torrents)
{
    if (generation_.load(std::memory_order_relaxed) != generation)
        return;

    std::vector<domain::SearchHit> hits;
    hits.reserve(torrents.size());
    for (domain::Torrent& t : torrents) {
        domain::SearchHit hit;
        hit.torrent = std::move(t);
        hits.push_back(std::move(hit));
    }
    resultView_.setResults(std::move(hits));
}

Component TopTab::component()
{
    MenuOption typeOption = MenuOption::Toggle();
    typeOption.entries = &kContentTypeLabels;
    typeOption.selected = &typeIndex_;
    typeOption.on_change = [this] { reload(); };
    Component typeToggle = Menu(typeOption);

    MenuOption timeOption = MenuOption::Toggle();
    timeOption.entries = &kTimeLabels;
    timeOption.selected = &timeIndex_;
    timeOption.on_change = [this] { reload(); };
    Component timeToggle = Menu(timeOption);

    Component topRow = Container::Horizontal({ typeToggle, timeToggle });
    Component topRowView = Renderer(
        topRow, [typeToggle, timeToggle] { return hbox({ typeToggle->Render(), text("  "), timeToggle->Render() }); });

    Component resultsMenu = resultView_.menu();
    Component layout = Container::Vertical({ topRow, resultsMenu });

    root_ = Renderer(layout, [this, topRowView, resultsMenu] {
        return vbox({
            topRowView->Render(),
            separator(),
            resultView_.renderPane(resultsMenu) | flex,
        });
    });

    root_ = CatchEvent(root_, [this](Event event) { return resultView_.handleKey(event); });

    return root_;
}

} // namespace ratsn::tui
