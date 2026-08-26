#include "engine/feed.h"

#include "domain/torrent_codec.h"
#include "platform/engine_loop.h"
#include "platform/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ratsn::engine {

using domain::ContentCategory;
using domain::ContentType;
using domain::Torrent;

namespace {

// Debounce window for coalescing feed writes into a single file rewrite.
constexpr int kFlushIntervalMs = 2000;

int64_t nowSecs()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Bad or xxx content never enters the feed (matches the legacy filter).
bool shouldBlock(const Torrent& torrent)
{
    return torrent.contentType == ContentType::Bad || torrent.contentCategory == ContentCategory::XXX;
}

// Stored/wire shape: the torrent JSON plus the extra feedDate. Both the file
// list and info are intentionally omitted to keep the feed payload lean -- a
// full feed carries up to maxSize_ torrents, and embedding every file list
// would bloat each P2P feed reply (and the stored file) into megabytes. The
// torrent's file *count* still travels (the codec always emits "files"); the
// file list itself is fetched on demand by hash when a torrent is opened.
librats::Json itemToJson(const FeedItem& item)
{
    librats::Json obj = domain::codec::toJson(item.torrent, { /*includeFiles*/ false, /*includeInfo*/ false });
    obj["feedDate"] = item.feedDate;
    return obj;
}

FeedItem itemFromJson(const librats::Json& obj)
{
    FeedItem item;
    item.torrent = domain::codec::torrentFromJson(obj);
    item.feedDate = obj.value("feedDate", int64_t { 0 });
    return item;
}

} // namespace

Feed::Feed(index::SearchIndex& index, platform::EngineLoop& engineLoop, std::string filePath)
    : index_(index), engineLoop_(engineLoop), filePath_(std::move(filePath))
{
}

Feed::~Feed()
{
    if (dirty_)
        persistNow();
}

bool Feed::load()
{
    librats::Json stored = librats::Json::array();
    std::ifstream in(filePath_, std::ios::binary);
    if (in) {
        std::ostringstream buf;
        buf << in.rdbuf();
        const std::string content = buf.str();
        if (!content.empty()) {
            librats::Json parsed = librats::Json::parse(content, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_array())
                stored = std::move(parsed);
            else
                platform::log() << "Feed: failed to parse feed file: " << filePath_ << "\n";
        }
    }

    feed_.clear();
    for (const librats::Json& value : stored) {
        if (!value.is_object())
            continue;
        FeedItem item = itemFromJson(value);
        if (item.torrent.hash.empty())
            continue;
        // Filter existing blocked content on load.
        if (shouldBlock(item.torrent)) {
            platform::log() << "Feed: filtering blocked content on load: " << item.torrent.hash.substr(0, 8) << "\n";
            continue;
        }
        feed_.push_back(std::move(item));
        if (static_cast<int>(feed_.size()) >= maxSize_)
            break;
    }

    reorder();

    feedDate_ = 0;
    for (const FeedItem& item : feed_)
        feedDate_ = std::max(feedDate_, item.feedDate);

    ++revision_;
    platform::log() << "Feed: loaded " << feed_.size() << " feed items\n";
    return true;
}

bool Feed::save()
{
    // Nothing changed since the last flush -- the stored file already
    // matches, so don't rewrite it (shutdown otherwise re-wrote the whole
    // feed every time).
    if (!dirty_)
        return true;
    return persistNow();
}

void Feed::add(const FeedItem& item)
{
    if (shouldBlock(item.torrent)) {
        platform::log() << "Feed: blocking item " << item.torrent.hash.substr(0, 8) << " - type: "
                         << domain::toString(item.torrent.contentType)
                         << " category: " << domain::toString(item.torrent.contentCategory) << "\n";
        return;
    }

    const auto existing = indexByHash_.find(item.torrent.hash);
    if (existing != indexByHash_.end()) {
        // Refresh votes/seeders, keep the original feedDate.
        FeedItem& current = feed_[existing->second];
        current.torrent.good = item.torrent.good;
        current.torrent.bad = item.torrent.bad;
        current.torrent.seeders = item.torrent.seeders;
    } else {
        FeedItem newItem = item;
        if (newItem.feedDate == 0)
            newItem.feedDate = nowSecs();

        if (static_cast<int>(feed_.size()) >= maxSize_) {
            // Drop the lowest-scored tail to make room, else replace the last.
            reorder();
            while (static_cast<int>(feed_.size()) >= maxSize_ && !feed_.empty()) {
                if (calculateScore(feed_.back()) <= 0) {
                    feed_.pop_back();
                } else {
                    feed_.back() = newItem;
                    break;
                }
            }
            if (static_cast<int>(feed_.size()) < maxSize_)
                feed_.push_back(newItem);
        } else {
            feed_.push_back(newItem);
        }
    }

    reorder();
    feedDate_ = nowSecs();

    markDirty();
    ++revision_;
}

void Feed::addByHash(const std::string& hash)
{
    if (hash.length() != 40)
        return;

    index::SearchQuery lookup;
    lookup.text = hash;
    lookup.limit = 1;
    std::vector<domain::SearchHit> hits = index_.searchNames(lookup);
    if (hits.empty() || !hits.front().torrent.isValid())
        return;

    const Torrent& torrent = hits.front().torrent;
    if (shouldBlock(torrent)) {
        platform::log() << "Feed: blocking torrent " << hash.substr(0, 8) << " - type: " << domain::toString(torrent.contentType)
                         << " category: " << domain::toString(torrent.contentCategory) << "\n";
        return;
    }

    FeedItem item;
    item.torrent = torrent;
    add(item);
}

std::vector<FeedItem> Feed::getFeed(int index, int limit) const
{
    if (index < 0 || index >= static_cast<int>(feed_.size()))
        return {};

    const int endIndex = std::min(index + limit, static_cast<int>(feed_.size()));
    return std::vector<FeedItem>(feed_.begin() + index, feed_.begin() + endIndex);
}

librats::Json Feed::toJsonArray(int index, int limit) const
{
    librats::Json arr = librats::Json::array();
    for (const FeedItem& item : getFeed(index, limit))
        arr.push_back(itemToJson(item));
    return arr;
}

void Feed::fromJsonArray(const librats::Json& array, int64_t remoteFeedDate)
{
    feed_.clear();

    if (array.is_array()) {
        for (const librats::Json& value : array) {
            if (!value.is_object())
                continue;
            FeedItem item = itemFromJson(value);
            if (item.torrent.hash.empty())
                continue;
            if (shouldBlock(item.torrent)) {
                platform::log() << "Feed: filtering blocked content from P2P: " << item.torrent.hash.substr(0, 8) << "\n";
                continue;
            }
            feed_.push_back(std::move(item));
            if (static_cast<int>(feed_.size()) >= maxSize_)
                break;
        }
    }

    reorder();
    feedDate_ = remoteFeedDate > 0 ? remoteFeedDate : nowSecs();

    markDirty();
    ++revision_;
}

void Feed::reorder()
{
    // Decorate-sort-undecorate: score each item once instead of recomputing
    // the sqrt-based Wilson score (and a nowSecs() call) twice per
    // comparison. Scoring inside the comparator also risked breaking strict
    // weak ordering, since the time term could shift mid-sort.
    std::vector<size_t> order(feed_.size());
    std::vector<double> scores(feed_.size());
    for (size_t i = 0; i < feed_.size(); ++i) {
        order[i] = i;
        scores[i] = calculateScore(feed_[i]);
    }
    std::sort(order.begin(), order.end(), [&scores](size_t a, size_t b) { return scores[a] > scores[b]; });

    std::vector<FeedItem> sorted;
    sorted.reserve(feed_.size());
    for (size_t i : order)
        sorted.push_back(std::move(feed_[i]));
    feed_ = std::move(sorted);

    rebuildIndex();
}

void Feed::rebuildIndex()
{
    indexByHash_.clear();
    indexByHash_.reserve(feed_.size());
    for (size_t i = 0; i < feed_.size(); ++i)
        indexByHash_[feed_[i].torrent.hash] = i;
}

double Feed::calculateScore(const FeedItem& item) const
{
    // Ranking algorithm from the legacy feed.js _compare function, ported
    // digit-for-digit from feed_service.cpp's calculateScore.
    const int good = item.torrent.good;
    const int bad = item.torrent.bad;

    const int64_t now = nowSecs();
    int64_t age = now - item.feedDate;

    constexpr int64_t maxTime = 600000; // ~7 days in seconds
    if (age > maxTime)
        age = maxTime;

    const double relativeTime = static_cast<double>(maxTime - age) / maxTime;

    // Wilson score interval for the rating (95% confidence).
    auto wilsonScore = [](int positive, int negative) -> double {
        const int n = positive + negative;
        if (n == 0)
            return 0.0;

        const double z = 1.96;
        const double phat = static_cast<double>(positive) / n;
        const double denominator = 1 + z * z / n;
        const double numerator = phat + z * z / (2 * n) - z * std::sqrt((phat * (1 - phat) + z * z / (4 * n)) / n);

        return numerator / denominator;
    };

    return relativeTime * relativeTime + good * 1.5 * relativeTime - bad * 0.6 * relativeTime + wilsonScore(good, bad);
}

void Feed::markDirty()
{
    dirty_ = true;
    if (flushScheduled_)
        return;
    flushScheduled_ = true;
    engineLoop_.postDelayed(
        [this] {
            flushScheduled_ = false;
            flush();
        },
        kFlushIntervalMs);
}

void Feed::flush()
{
    if (dirty_)
        persistNow();
}

bool Feed::persistNow()
{
    dirty_ = false;

    const librats::Json arr = toStoredArray();

    const std::filesystem::path path(filePath_);
    std::error_code dirEc;
    std::filesystem::create_directories(path.parent_path(), dirEc);

    // Write-to-temp-then-rename, same idiom as session_store.cpp: a save
    // interrupted mid-write (crash, power loss) must never leave a
    // half-written feed file behind for the next launch to choke on.
    const std::filesystem::path tmp = filePath_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            platform::log() << "Feed: failed to save feed to " << filePath_ << "\n";
            return false;
        }
        out << arr.dump(2);
        if (!out) {
            platform::log() << "Feed: failed to write feed to " << filePath_ << "\n";
            return false;
        }
    }
    std::error_code renameEc;
    std::filesystem::rename(tmp, path, renameEc);
    if (renameEc) {
        platform::log() << "Feed: failed to finalize feed file " << filePath_ << "\n";
        return false;
    }

    return true;
}

librats::Json Feed::toStoredArray() const
{
    librats::Json arr = librats::Json::array();
    for (const FeedItem& item : feed_)
        arr.push_back(itemToJson(item));
    return arr;
}

} // namespace ratsn::engine
