#include "tui/search_tab.h"

#include "domain/content.h"
#include "tui/format.h"

#include <algorithm>
#include <cstdio>

using namespace ftxui;

namespace ratsn::tui {

namespace {
const std::vector<std::string> kSortLabels = { "relevance", "seeders", "added", "size", "name" };
constexpr const char* kSortKeys[] = { "", "seeders", "added", "size", "name" };
} // namespace

SearchTab::SearchTab(platform::EngineLoop& engineLoop, index::SearchIndex& index, ftxui::ScreenInteractive& screen)
    : engineLoop_(engineLoop)
    , index_(index)
    , screen_(screen)
{
}

void SearchTab::focusInput()
{
    if (inputComponent_)
        inputComponent_->TakeFocus();
}

bool SearchTab::inputFocused() const
{
    return inputComponent_ && inputComponent_->Focused();
}

void SearchTab::triggerSearch()
{
    const uint64_t gen = ++generation_;

    if (queryText_.empty()) {
        applyResults(gen, {});
        return;
    }

    index::SearchQuery q;
    q.text = queryText_;
    q.limit = 200;
    q.sort = kSortKeys[sortIndex_];
    const bool files = searchFiles_;

    // Debounced on the engine thread (§7: "search-as-you-type debounced
    // 300ms"). A superseded gen means a newer keystroke already fired
    // another one of these -- skip the query entirely rather than just
    // discarding its result, so a burst of keystrokes costs one query, not
    // one query each.
    engineLoop_.postDelayed(
        [this, gen, q, files] {
            if (generation_.load(std::memory_order_relaxed) != gen)
                return;
            std::vector<domain::SearchHit> hits = files ? index_.searchFiles(q) : index_.searchNames(q);
            screen_.Post([this, gen, hits = std::move(hits)]() mutable { applyResults(gen, std::move(hits)); });
        },
        300);
}

void SearchTab::applyResults(uint64_t generation, std::vector<domain::SearchHit> hits)
{
    if (generation_.load(std::memory_order_relaxed) != generation)
        return;

    results_ = std::move(hits);
    resultLines_.clear();
    resultLines_.reserve(results_.size());
    for (const domain::SearchHit& hit : results_)
        resultLines_.push_back(formatResultLine(hit));
    selected_ = results_.empty() ? 0 : std::min(selected_, static_cast<int>(results_.size()) - 1);
    magnetMessage_.clear();
}

std::string SearchTab::formatResultLine(const domain::SearchHit& hit) const
{
    const domain::Torrent& t = hit.torrent;
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%-50.50s  %6s  %5d seeds", t.name.c_str(), humanSize(t.size).c_str(), t.seeders);
    std::string line = buf;
    if (hit.remote)
        line += "  [peer]";
    return line;
}

Element SearchTab::renderDetails() const
{
    if (results_.empty() || selected_ < 0 || selected_ >= static_cast<int>(results_.size()))
        return text("No results") | dim;

    const domain::SearchHit& hit = results_[static_cast<size_t>(selected_)];
    const domain::Torrent& t = hit.torrent;

    Elements lines;
    lines.push_back(text(t.name) | bold);
    lines.push_back(text(t.hash) | dim);
    lines.push_back(separatorEmpty());
    lines.push_back(hbox({ text("size: " + humanSize(t.size) + "   "), text("files: " + std::to_string(t.files)) }));
    lines.push_back(hbox({ text("seeders: " + std::to_string(t.seeders) + "   "),
        text("leechers: " + std::to_string(t.leechers) + "   "), text("completed: " + std::to_string(t.completed)) }));
    lines.push_back(text("added: " + humanDate(t.added)));

    std::string typeCat = domain::toString(t.contentType);
    const std::string cat = domain::toString(t.contentCategory);
    if (!cat.empty())
        typeCat = typeCat.empty() ? cat : typeCat + " / " + cat;
    if (!typeCat.empty())
        lines.push_back(text("type: " + typeCat));

    lines.push_back(text("votes: +" + std::to_string(t.good) + " / -" + std::to_string(t.bad)));

    if (hit.fromFileMatch && !hit.matchingPaths.empty()) {
        lines.push_back(separatorEmpty());
        lines.push_back(text("matching files:") | dim);
        for (const std::string& p : hit.matchingPaths)
            lines.push_back(text("  " + p));
    } else if (!t.fileList.empty()) {
        lines.push_back(separatorEmpty());
        lines.push_back(text("files (" + std::to_string(t.fileList.size()) + "):") | dim);
        constexpr size_t kMaxShown = 30;
        size_t shown = 0;
        for (const domain::File& f : t.fileList) {
            if (shown >= kMaxShown) {
                lines.push_back(text("  ... and " + std::to_string(t.fileList.size() - kMaxShown) + " more") | dim);
                break;
            }
            lines.push_back(text("  " + f.path + "  (" + humanSize(f.size) + ")"));
            ++shown;
        }
    }

    if (!magnetMessage_.empty()) {
        lines.push_back(separatorEmpty());
        lines.push_back(text(magnetMessage_) | color(Color::Yellow));
    }

    return vbox(std::move(lines)) | frame;
}

Component SearchTab::component()
{
    InputOption inputOption;
    inputOption.content = &queryText_;
    inputOption.placeholder = "search torrents...";
    inputOption.multiline = false;
    inputOption.on_change = [this] { triggerSearch(); };
    inputOption.on_enter = [this] { triggerSearch(); };
    inputComponent_ = Input(inputOption);

    CheckboxOption filesOption;
    filesOption.label = "files";
    filesOption.checked = &searchFiles_;
    filesOption.on_change = [this] { triggerSearch(); };
    Component filesCheckbox = Checkbox(filesOption);

    MenuOption sortOption = MenuOption::Toggle();
    sortOption.entries = &kSortLabels;
    sortOption.selected = &sortIndex_;
    sortOption.on_change = [this] { triggerSearch(); };
    Component sortToggle = Menu(sortOption);

    Component topRow = Container::Horizontal({ inputComponent_, filesCheckbox, sortToggle });

    MenuOption resultsOption;
    resultsOption.entries = &resultLines_;
    resultsOption.selected = &selected_;
    Component resultsMenu = Menu(resultsOption);

    Component layout = Container::Vertical({ topRow, resultsMenu });

    root_ = Renderer(layout, [this, topRow, resultsMenu] {
        return vbox({
            topRow->Render(),
            separator(),
            hbox({
                resultsMenu->Render() | frame | size(WIDTH, LESS_THAN, 62),
                separator(),
                renderDetails() | flex,
            }) | flex,
        });
    });

    root_ = CatchEvent(root_, [this](Event event) {
        if (inputFocused())
            return false; // never steal keys the user is typing into the query
        if (event == Event::Character('m') && !results_.empty() && selected_ >= 0
            && selected_ < static_cast<int>(results_.size())) {
            magnetMessage_ = "magnet: " + results_[static_cast<size_t>(selected_)].torrent.magnetLink();
            return true;
        }
        return false;
    });

    return root_;
}

} // namespace ratsn::tui
