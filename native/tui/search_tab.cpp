#include "tui/search_tab.h"

#include "domain/content.h"
#include "engine/peer_api.h"
#include "tui/format.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

using namespace ftxui;

namespace ratsn::tui {

namespace {
const std::vector<std::string> kSortLabels = { "relevance", "seeders", "added", "size", "name" };
constexpr const char* kSortKeys[] = { "", "seeders", "added", "size", "name" };

std::string asciiLower(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// Tokenizes on whitespace after ASCII-casefolding and stripping any trailing
// '*' (the local prefix-match operator, docs/M5-PLAN.md item 1 -- a remote
// peer's own index applies no such syntax, so it's stripped rather than
// matched literally).
std::vector<std::string> tokenizeCasefold(std::string text)
{
    while (!text.empty() && text.back() == '*')
        text.pop_back();
    text = asciiLower(text);

    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
            ++i;
        const size_t start = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])))
            ++i;
        if (i > start)
            tokens.push_back(text.substr(start, i - start));
    }
    return tokens;
}

} // namespace

SearchTab::SearchTab(platform::EngineLoop& engineLoop, index::SearchIndex& index, ftxui::ScreenInteractive& screen,
    engine::PeerApi* peerApi, engine::NodeHost* nodeHost, engine::DownloadManager* downloads,
    const platform::Config& cfg, std::string dataDir)
    : engineLoop_(engineLoop)
    , index_(index)
    , screen_(screen)
    , peerApi_(peerApi)
    , resultView_(engineLoop, screen, nodeHost, downloads, std::move(dataDir))
    , strict_(cfg.strictSearch)
    , safe_(cfg.safeSearch)
{
    if (peerApi_) {
        peerApi_->setSearchResultCallback([this](const domain::SearchHit& hit) { onRemoteHit(hit); });
        peerApi_->setFileSearchResultCallback([this](const domain::SearchHit& hit) { onRemoteHit(hit); });
    }
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

bool SearchTab::anyInputFocused() const
{
    return inputFocused() || (sizeMinInput_ && sizeMinInput_->Focused()) || (sizeMaxInput_ && sizeMaxInput_->Focused())
        || (seedersMinInput_ && seedersMinInput_->Focused()) || (trackerInput_ && trackerInput_->Focused());
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
    q.strict = strict_;
    q.safeSearch = safe_;
    q.contentType = kContentTypeValues[static_cast<size_t>(typeIndex_)];
    q.sizeMin = parseSize(sizeMinText_);
    q.sizeMax = parseSize(sizeMaxText_);
    q.seedersMin = std::atoi(seedersMinText_.c_str());
    q.tracker = trackerText_;
    const bool files = searchFiles_;

    // Debounced on the engine thread (§7: "search-as-you-type debounced
    // 300ms"). A superseded gen means a newer keystroke already fired
    // another one of these -- skip the query entirely rather than just
    // discarding its result, so a burst of keystrokes costs one query, not
    // one query each. The remote fan-out (M4-PLAN "Remote search merge")
    // rides the same debounce.
    engineLoop_.postDelayed(
        [this, gen, q, files] {
            if (generation_.load(std::memory_order_relaxed) != gen)
                return;
            std::vector<domain::SearchHit> hits = files ? index_.searchFiles(q) : index_.searchNames(q);
            screen_.Post([this, gen, hits = std::move(hits)]() mutable { applyResults(gen, std::move(hits)); });

            if (peerApi_) {
                remoteSearchGeneration_ = gen;
                remoteFilters_ = q;
                peerApi_->broadcastSearch(q.text, q.limit, q.sort, q.descending, q.safeSearch, q.contentType, files);
            }
        },
        300);
}

bool SearchTab::passesStrictTokenCheck(const domain::SearchHit& hit) const
{
    const std::vector<std::string> tokens = tokenizeCasefold(remoteFilters_.text);
    if (tokens.empty())
        return true;

    auto containsAll = [&tokens](const std::string& haystackLower) {
        for (const std::string& tok : tokens) {
            if (haystackLower.find(tok) == std::string::npos)
                return false;
        }
        return true;
    };

    if (containsAll(asciiLower(hit.torrent.name)))
        return true;
    for (const std::string& p : hit.matchingPaths) {
        if (containsAll(asciiLower(p)))
            return true;
    }
    return false;
}

bool SearchTab::passesLocalFilters(const domain::SearchHit& hit) const
{
    const domain::Torrent& t = hit.torrent;
    const index::SearchQuery& f = remoteFilters_;

    if (f.safeSearch && t.contentCategory == domain::ContentCategory::XXX)
        return false;

    if (!f.contentType.empty()) {
        if (f.contentType == "application") {
            if (t.contentType != domain::ContentType::Software && t.contentType != domain::ContentType::Games)
                return false;
        } else if (domain::toString(t.contentType) != f.contentType) {
            return false;
        }
    }

    if (f.sizeMin > 0 && t.size < f.sizeMin)
        return false;
    if (f.sizeMax > 0 && t.size > f.sizeMax)
        return false;
    if (f.filesMin > 0 && t.files < f.filesMin)
        return false;
    if (f.filesMax > 0 && t.files > f.filesMax)
        return false;
    if (f.seedersMin > 0 && t.seeders < f.seedersMin)
        return false;

    if (!f.tracker.empty()) {
        bool found = false;
        if (t.info.is_object()) {
            if (const librats::Json* trackers = t.info.as_object().find("trackers"); trackers && trackers->is_array()) {
                const std::string wanted = asciiLower(f.tracker);
                for (const librats::Json& v : *trackers) {
                    if (v.is_string() && asciiLower(v.get<std::string>()) == wanted) {
                        found = true;
                        break;
                    }
                }
            }
        }
        if (!found)
            return false;
    }

    return true;
}

void SearchTab::onRemoteHit(domain::SearchHit hit)
{
    // No correlation id on the wire (peer_api.cpp) -- a hit is accepted only
    // if no newer local query has started since we last broadcast one.
    const uint64_t gen = remoteSearchGeneration_;
    if (generation_.load(std::memory_order_relaxed) != gen)
        return;

    // Client-side filtering the wire can't carry (docs/M5-PLAN.md items 1/2):
    // done here, on the EngineLoop thread, against remoteFilters_ (this tab's
    // own state, never touched off this thread -- see its declaration).
    if (remoteFilters_.strict && !passesStrictTokenCheck(hit))
        return;
    if (!passesLocalFilters(hit))
        return;

    screen_.Post([this, gen, hit = std::move(hit)] {
        if (generation_.load(std::memory_order_relaxed) != gen)
            return;
        if (resultView_.hasHash(hit.torrent.hash))
            return; // dedup by hash against hits already shown
        resultView_.append(std::move(hit));
    });
}

void SearchTab::applyResults(uint64_t generation, std::vector<domain::SearchHit> hits)
{
    if (generation_.load(std::memory_order_relaxed) != generation)
        return;
    resultView_.setResults(std::move(hits));
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
    // Container::Horizontal's default render butts its children's renders
    // together with no gap -- the checkbox's "files" label runs straight
    // into the toggle's current entry ("relevance"/"seeders"/...) with
    // nothing between them. Wraps (doesn't replace) topRow purely for
    // drawing, via the same Renderer(child, fn)-forwards-events idiom root_
    // itself uses below, so Tab-focus cycling between the three controls is
    // unaffected.
    Component topRowView = Renderer(topRow, [this, filesCheckbox, sortToggle] {
        return hbox({
            inputComponent_->Render(),
            text(" "),
            filesCheckbox->Render(),
            text("  "),
            sortToggle->Render(),
        });
    });

    // --- Filter row (docs/M5-PLAN.md item 2), collapsed by default,
    // toggled with 'f' ---
    MenuOption typeOption = MenuOption::Toggle();
    typeOption.entries = &kContentTypeLabels;
    typeOption.selected = &typeIndex_;
    typeOption.on_change = [this] { triggerSearch(); };
    Component typeToggle = Menu(typeOption);

    CheckboxOption safeOption;
    safeOption.label = "safe";
    safeOption.checked = &safe_;
    safeOption.on_change = [this] { triggerSearch(); };
    Component safeCheckbox = Checkbox(safeOption);

    CheckboxOption strictOption;
    strictOption.label = "strict";
    strictOption.checked = &strict_;
    strictOption.on_change = [this] { triggerSearch(); };
    Component strictCheckbox = Checkbox(strictOption);

    InputOption sizeMinOption;
    sizeMinOption.content = &sizeMinText_;
    sizeMinOption.placeholder = "size>";
    sizeMinOption.on_change = [this] { triggerSearch(); };
    sizeMinInput_ = Input(sizeMinOption);

    InputOption sizeMaxOption;
    sizeMaxOption.content = &sizeMaxText_;
    sizeMaxOption.placeholder = "size<";
    sizeMaxOption.on_change = [this] { triggerSearch(); };
    sizeMaxInput_ = Input(sizeMaxOption);

    InputOption seedersOption;
    seedersOption.content = &seedersMinText_;
    seedersOption.placeholder = "seeders>";
    seedersOption.on_change = [this] { triggerSearch(); };
    seedersMinInput_ = Input(seedersOption);

    InputOption trackerOption;
    trackerOption.content = &trackerText_;
    trackerOption.placeholder = "tracker";
    trackerOption.on_change = [this] { triggerSearch(); };
    trackerInput_ = Input(trackerOption);

    Component filterRow = Container::Horizontal(
        { typeToggle, safeCheckbox, strictCheckbox, sizeMinInput_, sizeMaxInput_, seedersMinInput_, trackerInput_ });
    Component filterRowView = Renderer(filterRow, [this, typeToggle, safeCheckbox, strictCheckbox] {
        return hbox({
            typeToggle->Render(),
            text("  "),
            safeCheckbox->Render(),
            text("  "),
            strictCheckbox->Render(),
            text("  size:"),
            sizeMinInput_->Render() | size(WIDTH, EQUAL, 8),
            text("-"),
            sizeMaxInput_->Render() | size(WIDTH, EQUAL, 8),
            text("  seeders>"),
            seedersMinInput_->Render() | size(WIDTH, EQUAL, 6),
            text("  tracker:"),
            trackerInput_->Render() | size(WIDTH, EQUAL, 10),
        });
    });
    // Maybe (not just conditional rendering) so a hidden filter row's Inputs
    // can't take focus/keys either -- see anyInputFocused() and the 'f'
    // guard below.
    Component filterRowMaybe = Maybe(filterRowView, &filtersVisible_);

    Component resultsMenu = resultView_.menu();

    Component layout = Container::Vertical({ topRow, filterRowMaybe, resultsMenu });

    root_ = Renderer(layout, [this, topRowView, filterRowMaybe, resultsMenu] {
        return vbox({
            topRowView->Render(),
            filterRowMaybe->Render(),
            separator(),
            resultView_.renderPane(resultsMenu) | flex,
        });
    });

    root_ = CatchEvent(root_, [this](Event event) {
        if (anyInputFocused())
            return false; // never steal keys the user is typing into a query/filter input
        if (event == Event::Character('f')) {
            filtersVisible_ = !filtersVisible_;
            return true;
        }
        return resultView_.handleKey(event);
    });

    return root_;
}

} // namespace ratsn::tui
