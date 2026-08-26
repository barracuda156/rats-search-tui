#include "engine/tracker_service.h"

#include "engine/site_scraper.h"
#include "engine/swarm_scraper.h"
#include "index/search_index.h"

#include <chrono>

namespace ratsn::engine {

namespace {
int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

TrackerService::TrackerService(SwarmScraper& swarmScraper, TrackerSiteScraper& siteScraper, index::SearchIndex& index)
    : swarmScraper_(swarmScraper)
    , siteScraper_(siteScraper)
    , index_(index)
{
    swarmScraper_.setScrapedCallback([this](const std::string& hash, int seeders, int leechers, int completed) {
        onCountsScraped(hash, seeders, leechers, completed);
    });
    siteScraper_.setScrapedCallback(
        [this](const std::string& hash, const librats::Json& info) { onInfoScraped(hash, info); });
}

void TrackerService::stop()
{
    // Stop forwarding first so a late onTorrentIndexed/checkX call issues
    // nothing, then drain the scrapers (blocking announces / in-flight HTTP).
    countEnabled_ = false;
    infoEnabled_ = false;
    swarmScraper_.stop();
    siteScraper_.stop();
}

void TrackerService::checkCounts(const std::string& hash)
{
    if (countEnabled_)
        swarmScraper_.requestScrape(hash);
}

void TrackerService::checkInfo(const std::string& hash, const std::string& name)
{
    if (infoEnabled_)
        siteScraper_.scrape(hash, name);
}

void TrackerService::onTorrentIndexed(const std::string& hash, const std::string& name)
{
    checkCounts(hash);
    checkInfo(hash, name);
}

void TrackerService::onCountsScraped(const std::string& hash, int seeders, int leechers, int completed)
{
    if (!index_.updateStats(hash, seeders, leechers, completed))
        return;
    if (onStatsUpdated_)
        onStatsUpdated_(hash, seeders, leechers, completed, nowMs());
}

void TrackerService::onInfoScraped(const std::string& hash, const librats::Json& info)
{
    if (!index_.mergeInfo(hash, info))
        return;
    if (onInfoUpdated_)
        onInfoUpdated_(hash, info);
}

} // namespace ratsn::engine
