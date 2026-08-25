#include "tui/downloads_tab.h"

#include "platform/log.h"
#include "tui/format.h"

#include <algorithm>
#include <cstdio>

using namespace ftxui;

namespace ratsn::tui {

namespace {
// A double-'X' press within this window deletes files (docs/M6-PLAN.md
// deviation #3); otherwise it just (re-)arms the confirmation.
constexpr auto kDeleteConfirmWindow = std::chrono::seconds(5);

std::string stateLabel(const engine::Download& d)
{
    if (d.completed)
        return d.progress >= 1.0 ? "seeding" : "completed";
    if (d.paused)
        return "paused";
    if (!d.ready)
        return "fetching metadata";
    return "downloading";
}
} // namespace

DownloadsTab::DownloadsTab(
    platform::EngineLoop& engineLoop, ftxui::ScreenInteractive& screen, engine::DownloadManager* downloads)
    : engineLoop_(engineLoop)
    , screen_(screen)
    , downloads_(downloads)
{
}

bool DownloadsTab::inputFocused() const { return addInput_ && addInput_->Focused(); }

std::string DownloadsTab::formatRow(const engine::Download& d) const
{
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%-38.38s %5.1f%%  %8s/%-8s  %8s/s  %3d peers  %s", d.name.c_str(),
        d.progress * 100.0, humanSize(d.downloadedBytes).c_str(), humanSize(d.totalSize).c_str(),
        humanSize(static_cast<int64_t>(d.downloadSpeed)).c_str(), d.peersConnected, stateLabel(d).c_str());
    return buf;
}

Element DownloadsTab::renderDetails() const
{
    if (rows_.empty() || selected_ < 0 || selected_ >= static_cast<int>(rows_.size()))
        return text("No downloads -- type a magnet link, hash or .torrent path above and press Enter") | dim;

    const engine::Download& d = rows_[static_cast<size_t>(selected_)];

    Elements lines;
    lines.push_back(text(d.name) | bold);
    lines.push_back(text(d.hash) | dim);
    lines.push_back(separatorEmpty());
    lines.push_back(gauge(static_cast<float>(d.progress)) | color(d.completed ? Color::Green : Color::Blue));
    lines.push_back(hbox({
        text(humanSize(d.downloadedBytes) + " / " + humanSize(d.totalSize) + "   "),
        text(std::to_string(static_cast<int>(d.progress * 100)) + "%   "),
        text(humanSize(static_cast<int64_t>(d.downloadSpeed)) + "/s   "),
        text(std::to_string(d.peersConnected) + " peers"),
    }));
    lines.push_back(text(stateLabel(d)) | (d.paused ? color(Color::Yellow) : color(Color::Default)));
    lines.push_back(text("save path: " + d.savePath) | dim);

    if (!d.files.empty()) {
        lines.push_back(separatorEmpty());
        lines.push_back(text("files (" + std::to_string(d.files.size()) + "):") | dim);
        constexpr size_t kMaxShown = 30;
        size_t shown = 0;
        for (const engine::DownloadFile& f : d.files) {
            if (shown >= kMaxShown) {
                lines.push_back(text("  ... and " + std::to_string(d.files.size() - kMaxShown) + " more") | dim);
                break;
            }
            lines.push_back(text("  " + f.path + "  (" + humanSize(f.size) + ")"));
            ++shown;
        }
    }

    return vbox(std::move(lines)) | frame;
}

void DownloadsTab::applySnapshot(std::vector<engine::Download> rows)
{
    rows_ = std::move(rows);
    // DownloadManager::snapshot() iterates a std::map (already hash-order);
    // sorted explicitly here so rows don't reorder if that ever changes.
    std::sort(rows_.begin(), rows_.end(),
        [](const engine::Download& a, const engine::Download& b) { return a.hash < b.hash; });

    rowLines_.clear();
    rowLines_.reserve(rows_.size());
    for (const engine::Download& d : rows_)
        rowLines_.push_back(formatRow(d));
    selected_ = rows_.empty() ? 0 : std::min(selected_, static_cast<int>(rows_.size()) - 1);
}

void DownloadsTab::schedule()
{
    engineLoop_.postDelayed([this] { tick(); }, 1000);
}

void DownloadsTab::tick()
{
    if (!engineLoop_.isRunning())
        return;

    if (downloads_) {
        const uint64_t rev = downloads_->revision();
        // Skip the post (and the redraw it triggers) when nothing about the
        // displayed state actually moved -- see DownloadManager::revision().
        if (rev != lastRevision_) {
            lastRevision_ = rev;
            std::vector<engine::Download> snap = downloads_->snapshot();
            screen_.Post([this, snap = std::move(snap)]() mutable { applySnapshot(std::move(snap)); });
        }
    }

    schedule();
}

void DownloadsTab::start() { schedule(); }

void DownloadsTab::handleAddSubmit()
{
    std::string text = addText_;
    addText_.clear();
    if (text.empty())
        return;

    if (!downloads_) {
        statusMessage_ = "add failed: bittorrent not available (spider/mesh disabled)";
        return;
    }

    statusMessage_ = "adding: " + text;
    platform::log() << "DownloadsTab: add requested: " << text << "\n";

    engine::DownloadManager* downloads = downloads_;
    const bool asTorrentFile = text.size() > 8 && text.compare(text.size() - 8, 8, ".torrent") == 0;
    engineLoop_.post([this, downloads, text, asTorrentFile] {
        const bool ok = asTorrentFile ? downloads->addFromFile(text) : downloads->add(text);
        screen_.Post([this, ok, text] {
            statusMessage_ = ok ? ("added: " + text) : ("failed to add (invalid, unreadable or already downloading): " + text);
        });
    });
}

bool DownloadsTab::handleKey(ftxui::Event event)
{
    if (rows_.empty() || selected_ < 0 || selected_ >= static_cast<int>(rows_.size()))
        return false;

    const engine::Download& d = rows_[static_cast<size_t>(selected_)];
    const std::string hash = d.hash;

    if (event == Event::Character(' ')) {
        statusMessage_ = (d.paused ? "resuming: " : "pausing: ") + hash.substr(0, 8) + "...";
        if (downloads_) {
            engine::DownloadManager* downloads = downloads_;
            engineLoop_.post([downloads, hash] { downloads->togglePause(hash); });
        }
        return true;
    }
    if (event == Event::Character('x')) {
        statusMessage_ = "removed (files kept): " + hash.substr(0, 8) + "...";
        armedDeleteHash_.clear();
        if (downloads_) {
            engine::DownloadManager* downloads = downloads_;
            engineLoop_.post([downloads, hash] { downloads->remove(hash, /*saveResumeData=*/true); });
        }
        return true;
    }
    if (event == Event::Character('X')) {
        const auto now = std::chrono::steady_clock::now();
        if (armedDeleteHash_ == hash && now - armedAt_ < kDeleteConfirmWindow) {
            statusMessage_ = "deleted: " + hash.substr(0, 8) + "...";
            armedDeleteHash_.clear();
            if (downloads_) {
                engine::DownloadManager* downloads = downloads_;
                engineLoop_.post([downloads, hash] { downloads->removeAndDelete(hash); });
            }
        } else {
            armedDeleteHash_ = hash;
            armedAt_ = now;
            statusMessage_ = "press X again within 5s to delete files for " + hash.substr(0, 8) + "...";
        }
        return true;
    }
    return false;
}

Component DownloadsTab::component()
{
    InputOption addOption;
    addOption.content = &addText_;
    addOption.placeholder = "magnet link / hash / .torrent path -- Enter to add";
    addOption.multiline = false;
    addOption.on_enter = [this] { handleAddSubmit(); };
    addInput_ = Input(addOption);

    MenuOption listOption;
    listOption.entries = &rowLines_;
    listOption.selected = &selected_;
    Component list = Menu(listOption);

    Component layout = Container::Vertical({ addInput_, list });

    root_ = Renderer(layout, [this, list] {
        Elements rightColumn;
        rightColumn.push_back(renderDetails() | flex);
        if (!statusMessage_.empty()) {
            rightColumn.push_back(separator());
            rightColumn.push_back(text(statusMessage_) | color(Color::Yellow));
        }

        return vbox({
            hbox({ text("add: "), addInput_->Render() | flex }),
            separator(),
            hbox({
                // yframe (not frame): see ResultView::renderPane for why --
                // same fixed-width truncated rows here.
                list->Render() | yframe | size(WIDTH, LESS_THAN, 70),
                separator(),
                vbox(std::move(rightColumn)) | flex,
            }) | flex,
        });
    });

    root_ = CatchEvent(root_, [this](Event event) {
        if (inputFocused())
            return false; // never steal keys the user is typing into the add row
        return handleKey(event);
    });

    return root_;
}

} // namespace ratsn::tui
