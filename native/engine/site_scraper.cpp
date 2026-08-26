#include "engine/site_scraper.h"

#include "platform/engine_loop.h"
#include "platform/log.h"
#include "platform/worker_pool.h"

#include <curl/curl.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>

namespace ratsn::engine {

namespace {

// User-Agent shared by every request; trackers gate some content on it.
constexpr const char* kUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string& s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string join(const std::vector<std::string>& parts, const std::string& sep)
{
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0)
            out += sep;
        out += parts[i];
    }
    return out;
}

bool containsCaseInsensitive(const std::string& haystack, const std::string& needleLower)
{
    // Equivalent to Qt's `contains("windows-1251") || contains("charset=windows-1251")`
    // -- the second clause there is a strict subset of the first (any string
    // containing "charset=windows-1251" also contains "windows-1251").
    return toLower(haystack).find(needleLower) != std::string::npos;
}

// Everything below operates on raw HTML bytes without PCRE2_UTF: every
// pattern anchors on plain ASCII HTML syntax (tags, attribute names), and
// multi-byte UTF-8 sequences inside `.`-matched spans (titles, descriptions)
// simply pass through untouched -- matches are never split mid-sequence
// because they're delimited by ASCII tag boundaries.

// Runs `pattern` against `subject`; returns captured groups by index (0 =
// whole match) from the first match, or an empty vector if it didn't match
// or the pattern failed to compile.
std::vector<std::string> firstMatch(const std::string& pattern, const std::string& subject, uint32_t options = 0)
{
    std::vector<std::string> groups;

    int errorCode = 0;
    PCRE2_SIZE errorOffset = 0;
    pcre2_code* code = pcre2_compile(
        reinterpret_cast<PCRE2_SPTR>(pattern.c_str()), PCRE2_ZERO_TERMINATED, options, &errorCode, &errorOffset, nullptr);
    if (!code)
        return groups;

    pcre2_match_data* match = pcre2_match_data_create_from_pattern(code, nullptr);
    const int rc = pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(subject.c_str()), subject.size(), 0, 0, match, nullptr);
    if (rc > 0) {
        const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(match);
        groups.reserve(static_cast<size_t>(rc));
        for (int i = 0; i < rc; ++i) {
            const PCRE2_SIZE start = ovector[2 * i];
            const PCRE2_SIZE end = ovector[2 * i + 1];
            if (start == PCRE2_UNSET || end == PCRE2_UNSET)
                groups.emplace_back();
            else
                groups.push_back(subject.substr(start, end - start));
        }
    }
    pcre2_match_data_free(match);
    pcre2_code_free(code);
    return groups;
}

// Global regex replace -- PCRE2's substitute API needs a version new enough
// to auto-grow its output buffer, which isn't guaranteed on the retro
// target's PCRE2, so this walks matches manually instead.
std::string replaceAllRegex(const std::string& subject, const std::string& pattern, const std::string& replacement, uint32_t options = 0)
{
    int errorCode = 0;
    PCRE2_SIZE errorOffset = 0;
    pcre2_code* code = pcre2_compile(
        reinterpret_cast<PCRE2_SPTR>(pattern.c_str()), PCRE2_ZERO_TERMINATED, options, &errorCode, &errorOffset, nullptr);
    if (!code)
        return subject;

    pcre2_match_data* match = pcre2_match_data_create_from_pattern(code, nullptr);
    const auto* subjectData = reinterpret_cast<PCRE2_SPTR>(subject.data());
    const PCRE2_SIZE subjectLen = subject.size();

    std::string result;
    result.reserve(subject.size());
    PCRE2_SIZE pos = 0;

    for (;;) {
        const int rc = pcre2_match(code, subjectData, subjectLen, pos, 0, match, nullptr);
        if (rc < 0)
            break;
        const PCRE2_SIZE* ov = pcre2_get_ovector_pointer(match);
        const PCRE2_SIZE mStart = ov[0];
        const PCRE2_SIZE mEnd = ov[1];
        result.append(subject, pos, mStart - pos);
        result.append(replacement);
        if (mEnd > pos) {
            pos = mEnd;
        } else {
            // Zero-length match: copy one byte forward to guarantee progress.
            if (mEnd < subjectLen)
                result.push_back(subject[mEnd]);
            pos = mEnd + 1;
        }
        if (pos > subjectLen)
            break;
    }
    if (pos <= subjectLen)
        result.append(subject, pos, std::string::npos);

    pcre2_match_data_free(match);
    pcre2_code_free(code);
    return result;
}

std::string utf8Encode(unsigned long codepoint)
{
    std::string out;
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    return out;
}

// Decodes "&#NNN;" numeric character references into their UTF-8 encoding --
// port of the QRegularExpressionMatchIterator loop in stripHtml, which
// appended QChar(code) directly (UTF-16); we build the equivalent UTF-8 bytes
// ourselves since std::string carries UTF-8 here.
std::string decodeNumericEntities(const std::string& text)
{
    static const std::string kPattern = R"(&#(\d+);)";
    int errorCode = 0;
    PCRE2_SIZE errorOffset = 0;
    pcre2_code* code = pcre2_compile(
        reinterpret_cast<PCRE2_SPTR>(kPattern.c_str()), PCRE2_ZERO_TERMINATED, 0, &errorCode, &errorOffset, nullptr);
    if (!code)
        return text;

    pcre2_match_data* match = pcre2_match_data_create_from_pattern(code, nullptr);
    const auto* subjectData = reinterpret_cast<PCRE2_SPTR>(text.data());
    const PCRE2_SIZE subjectLen = text.size();

    std::string result;
    result.reserve(text.size());
    PCRE2_SIZE pos = 0;

    for (;;) {
        const int rc = pcre2_match(code, subjectData, subjectLen, pos, 0, match, nullptr);
        if (rc <= 0)
            break;
        const PCRE2_SIZE* ov = pcre2_get_ovector_pointer(match);
        const PCRE2_SIZE mStart = ov[0];
        const PCRE2_SIZE mEnd = ov[1];
        result.append(text, pos, mStart - pos);
        if (rc > 1 && ov[2] != PCRE2_UNSET) {
            const long codepoint = std::strtol(text.substr(ov[2], ov[3] - ov[2]).c_str(), nullptr, 10);
            if (codepoint > 0 && codepoint < 0x10FFFF)
                result += utf8Encode(static_cast<unsigned long>(codepoint));
        }
        pos = mEnd > pos ? mEnd : pos + 1;
        if (pos > subjectLen)
            break;
    }
    if (pos <= subjectLen)
        result.append(text, pos, std::string::npos);

    pcre2_match_data_free(match);
    pcre2_code_free(code);
    return result;
}

// Skips "scheme://host" to find where a URL's path begins, then checks it
// against `prefix` -- used to detect Nyaa's search->view redirect the same
// way Qt checked `reply->url().path().startsWith("/view/")`.
bool urlPathStartsWith(const std::string& url, const std::string& prefix)
{
    const size_t schemeEnd = url.find("://");
    const size_t hostStart = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
    const size_t pathStart = url.find('/', hostStart);
    if (pathStart == std::string::npos)
        return false;
    return url.compare(pathStart, prefix.size(), prefix) == 0;
}

size_t curlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

// Nonzero aborts the transfer -- the cancellation path TrackerSiteScraper::
// stop() relies on (docs/M8-PLAN.md item 2).
int curlProgressCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return static_cast<std::atomic<bool>*>(clientp)->load(std::memory_order_relaxed) ? 1 : 0;
}

} // namespace

// The authoritative strategy list. Its size -- not a hardcoded constant --
// is what every scrape uses as its pending-result count.
const std::vector<TrackerSiteScraper::Strategy> TrackerSiteScraper::kStrategies = {
    &TrackerSiteScraper::scrapeRutracker,
    &TrackerSiteScraper::scrapeNyaa,
};

TrackerSiteScraper::TrackerSiteScraper(platform::EngineLoop& engineLoop)
    : engineLoop_(engineLoop)
    , pool_(std::make_unique<platform::WorkerPool>(kMaxPoolThreads))
{
}

TrackerSiteScraper::~TrackerSiteScraper()
{
    stop();
}

void TrackerSiteScraper::stop()
{
    stopping_.store(true, std::memory_order_relaxed);
    pendingQueue_.clear();
    if (pool_)
        pool_->stopAndDrain();
}

void TrackerSiteScraper::scrape(const std::string& infoHash, const std::string& name)
{
    if (stopping_.load(std::memory_order_relaxed))
        return; // shutting down -- accept no new work

    if (infoHash.size() != static_cast<size_t>(kInfoHashHexLength))
        return;

    const auto now = std::chrono::steady_clock::now();
    if (const auto it = recentChecks_.find(infoHash); it != recentChecks_.end()) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
        if (elapsed < kCooldownSecs) {
            platform::log() << "TrackerSiteScraper: Hash " << infoHash.substr(0, 8) << " checked recently, skipping\n";
            return;
        }
    }
    recentChecks_[infoHash] = now;

    if (activeRequests_ >= kMaxConcurrent) {
        pendingQueue_.push_back({ infoHash, name });
        platform::log() << "TrackerSiteScraper: queued " << infoHash.substr(0, 8) << " - active:" << activeRequests_
                         << " queued:" << pendingQueue_.size() << "\n";
        return;
    }
    ++activeRequests_;

    startScrape(infoHash, name);
}

void TrackerSiteScraper::startScrape(const std::string& infoHash, const std::string& name)
{
    PendingScrape pending;
    pending.name = name;
    pending.pendingCount = static_cast<int>(kStrategies.size());
    pendingScrapes_[infoHash] = std::move(pending);

    platform::log() << "TrackerSiteScraper: Scraping tracker info for " << infoHash.substr(0, 16) << " "
                     << name.substr(0, 48) << "\n";

    for (const Strategy strategy : kStrategies)
        (this->*strategy)(infoHash);
}

void TrackerSiteScraper::processQueue()
{
    if (stopping_.load(std::memory_order_relaxed))
        return;

    while (!pendingQueue_.empty() && activeRequests_ < kMaxConcurrent) {
        PendingRequest req = std::move(pendingQueue_.front());
        pendingQueue_.pop_front();
        ++activeRequests_;
        startScrape(req.infoHash, req.name);
    }
}

// ============================================================================
// HTTP transport
// ============================================================================

TrackerSiteScraper::HttpResult TrackerSiteScraper::httpGet(const std::string& url, const std::vector<std::string>& extraHeaders)
{
    HttpResult result;

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "curl_easy_init failed";
        return result;
    }

    curl_slist* headers = nullptr;
    for (const std::string& h : extraHeaders)
        headers = curl_slist_append(headers, h.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // Empty string enables every decoder libcurl was built with (gzip/
    // deflate/br) -- the equivalent of Qt's transparent inflate.
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(kTimeoutMs));
    // No async DNS resolver is assumed on the retro target -- NOSIGNAL avoids
    // libcurl's signal-based timeout path, which is not thread-safe.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &curlProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &stopping_);

    const CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        result.ok = true;
        char* effectiveUrl = nullptr;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
        if (effectiveUrl)
            result.finalUrl = effectiveUrl;
    } else {
        result.error = curl_easy_strerror(rc);
    }

    if (headers)
        curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

// ============================================================================
// RuTracker strategy
// ============================================================================

void TrackerSiteScraper::scrapeRutracker(const std::string& hash)
{
    const std::string url = "https://rutracker.org/forum/viewtopic.php?h=" + hash;
    pool_->post([this, hash, url]() {
        HttpResult res = httpGet(url,
            { "Accept: text/html,application/xhtml+xml", "Accept-Language: ru-RU,ru;q=0.9,en-US;q=0.8,en;q=0.7" });
        engineLoop_.post([this, hash, res = std::move(res)]() {
            TrackerSiteInfo info;
            info.trackerName = "rutracker";
            if (res.ok)
                info = parseRutrackerHtml(res.body);
            else
                platform::log() << "TrackerSiteScraper: RuTracker request failed: " << res.error << "\n";
            onStrategyComplete(hash, info);
        });
    });
}

TrackerSiteInfo TrackerSiteScraper::parseRutrackerHtml(const std::string& rawData)
{
    TrackerSiteInfo info;
    info.trackerName = "rutracker";

    if (rawData.empty())
        return info;

    // RuTracker pages are typically windows-1251 encoded. Sniff the head of
    // the document for the charset before deciding how to decode.
    std::string html;
    const std::string preview = rawData.substr(0, std::min<size_t>(rawData.size(), kEncodingSniffLength));
    if (containsCaseInsensitive(preview, "windows-1251")) {
        platform::log() << "TrackerSiteScraper: Windows-1251 detected, decoding\n";
        html = decodeWindows1251(rawData);
    } else {
        html = rawData; // already UTF-8 (plain ASCII is a subset)
    }

    if (html.empty())
        return info;

    // Pre-process: add newlines before post-br and post-b (like legacy).
    html = replaceAllRegex(html, "<span class=\"post-br\">", "\n<span class=\"post-br\">");
    html = replaceAllRegex(html, "><span class=\"post-b\">", ">\n<span class=\"post-b\">");

    // Extract topic title: <a id="topic-title" ...>TITLE</a>
    if (auto m = firstMatch(R"(<a[^>]*id\s*=\s*"topic-title"[^>]*>(.*?)</a>)", html, PCRE2_DOTALL); m.size() > 1)
        info.name = trim(stripHtml(m[1]));

    // If no topic title found, the page didn't match -- return empty.
    if (info.name.empty())
        return info;

    // Extract poster image: <var|img class="postImg(Aligned)" title="URL">
    if (auto m = firstMatch(R"re(<(?:var|img)[^>]*class\s*=\s*"postImg(?:Aligned)?"[^>]*title\s*=\s*"([^"]+)")re", html);
        m.size() > 1)
        info.poster = trim(m[1]);

    // Extract description: first <div class="post_body"> ... </div>.
    if (auto m = firstMatch(R"(<div[^>]*class\s*=\s*"post_body"[^>]*>(.*?)</div\s*>\s*</td>)", html, PCRE2_DOTALL);
        m.size() > 1)
        info.description = trim(truncateDescription(stripHtml(m[1])));

    // Extract thread ID from the magnet link's data-topic_id attribute.
    if (auto m = firstMatch(R"re(<a[^>]*class\s*=\s*"magnet-link(?:-1)?"[^>]*data-topic_id\s*=\s*"(\d+)")re", html);
        m.size() > 1)
        info.threadId = std::atoi(m[1].c_str());

    // Extract content category from the navigation breadcrumb.
    if (auto m = firstMatch(
            R"(<td[^>]*class\s*=\s*"vBottom"[^>]*>.*?<[^>]*class\s*=\s*"nav"[^>]*>(.*?)</(?:div|td)>)", html, PCRE2_DOTALL);
        m.size() > 1) {
        std::string cat = trim(replaceAllRegex(stripHtml(m[1]), "\\s+", " "));
        if (!cat.empty())
            info.contentCategory = cat;
    }

    info.success = true;
    platform::log() << "TrackerSiteScraper: RuTracker found: " << info.name.substr(0, 60)
                     << " threadId:" << info.threadId << "\n";

    return info;
}

// ============================================================================
// Nyaa strategy
// ============================================================================

void TrackerSiteScraper::scrapeNyaa(const std::string& hash)
{
    const std::string url = "https://nyaa.si/?q=" + hash;
    pool_->post([this, hash, url]() {
        HttpResult res = httpGet(url, { "Accept: text/html,application/xhtml+xml" });
        engineLoop_.post([this, hash, res = std::move(res)]() {
            if (!res.ok) {
                platform::log() << "TrackerSiteScraper: Nyaa request failed: " << res.error << "\n";
                TrackerSiteInfo info;
                info.trackerName = "nyaa";
                onStrategyComplete(hash, info);
                return;
            }

            // A single search result redirects straight to the /view/ page.
            if (urlPathStartsWith(res.finalUrl, "/view/")) {
                onStrategyComplete(hash, parseNyaaViewHtml(res.body));
                return;
            }

            // Otherwise parse the search results to locate a view link.
            const TrackerSiteInfo searchInfo = parseNyaaSearchHtml(res.body);
            if (searchInfo.success && searchInfo.threadId > 0) {
                // Found a result -- fetch the view page for full details.
                scrapeNyaaViewPage(hash, "https://nyaa.si/view/" + std::to_string(searchInfo.threadId));
            } else if (searchInfo.success) {
                // Got some info directly from the search page.
                onStrategyComplete(hash, searchInfo);
            } else {
                // No results on Nyaa.
                TrackerSiteInfo emptyInfo;
                emptyInfo.trackerName = "nyaa";
                onStrategyComplete(hash, emptyInfo);
            }
        });
    });
}

void TrackerSiteScraper::scrapeNyaaViewPage(const std::string& hash, const std::string& viewUrl)
{
    if (stopping_.load(std::memory_order_relaxed)) {
        // Don't chain a fresh request during shutdown; close out the strategy.
        TrackerSiteInfo info;
        info.trackerName = "nyaa";
        onStrategyComplete(hash, info);
        return;
    }

    pool_->post([this, hash, viewUrl]() {
        HttpResult res = httpGet(viewUrl, { "Accept: text/html,application/xhtml+xml" });
        engineLoop_.post([this, hash, res = std::move(res)]() {
            TrackerSiteInfo info;
            info.trackerName = "nyaa";
            if (res.ok)
                info = parseNyaaViewHtml(res.body);
            else
                platform::log() << "TrackerSiteScraper: Nyaa view page request failed: " << res.error << "\n";
            onStrategyComplete(hash, info);
        });
    });
}

TrackerSiteInfo TrackerSiteScraper::parseNyaaSearchHtml(const std::string& rawData)
{
    TrackerSiteInfo info;
    info.trackerName = "nyaa";

    if (rawData.empty())
        return info;

    // Look for a view link in the search results:
    // <td ...><a href="/view/1234567" ...>Title</a></td>
    if (auto m = firstMatch(R"re(<a[^>]*href\s*=\s*"/view/(\d+)"[^>]*>([^<]+)</a>)re", rawData); m.size() > 2) {
        info.threadId = std::atoi(m[1].c_str());
        info.name = trim(m[2]);
        info.success = true;
    }

    // Also try the panel-title (single result / detail view).
    if (!info.success) {
        if (auto m = firstMatch(R"(<h3[^>]*class\s*=\s*"panel-title"[^>]*>(.*?)</h3>)", rawData, PCRE2_DOTALL);
            m.size() > 1) {
            std::string title = replaceAllRegex(trim(stripHtml(m[1])), "[\\t\\n]+", "");
            if (!title.empty() && title != "Nyaa") {
                info.name = title;
                info.success = true;
            }
        }
    }

    // Grab the description if we happen to be on a detail page.
    if (auto m = firstMatch(R"(<div[^>]*id\s*=\s*"torrent-description"[^>]*>(.*?)</div>)", rawData, PCRE2_DOTALL);
        m.size() > 1)
        info.description = truncateDescription(trim(stripHtml(m[1])));

    return info;
}

TrackerSiteInfo TrackerSiteScraper::parseNyaaViewHtml(const std::string& rawData)
{
    TrackerSiteInfo info;
    info.trackerName = "nyaa";

    if (rawData.empty())
        return info;

    // Extract title from panel-title.
    if (auto m = firstMatch(R"(<h3[^>]*class\s*=\s*"panel-title"[^>]*>(.*?)</h3>)", rawData, PCRE2_DOTALL);
        m.size() > 1) {
        std::string title = replaceAllRegex(trim(stripHtml(m[1])), "[\\t\\n]+", "");
        if (!title.empty() && title != "Nyaa") {
            info.name = title;
            info.success = true;
        }
    }

    if (!info.success)
        return info;

    // Extract description.
    if (auto m = firstMatch(R"(<div[^>]*id\s*=\s*"torrent-description"[^>]*>(.*?)</div>)", rawData, PCRE2_DOTALL);
        m.size() > 1)
        info.description = truncateDescription(trim(stripHtml(m[1])));

    // Extract view ID from a URL in the page (for thread linking).
    if (auto m = firstMatch(R"(/view/(\d+))", rawData); m.size() > 1)
        info.threadId = std::atoi(m[1].c_str());

    platform::log() << "TrackerSiteScraper: Nyaa found: " << info.name.substr(0, 60) << "\n";

    return info;
}

// ============================================================================
// Result merging
// ============================================================================

void TrackerSiteScraper::onStrategyComplete(const std::string& hash, const TrackerSiteInfo& info)
{
    auto it = pendingScrapes_.find(hash);
    if (it == pendingScrapes_.end())
        return;

    if (info.success)
        it->second.results.push_back(info);
    --it->second.pendingCount;

    checkAllComplete(hash);
}

void TrackerSiteScraper::checkAllComplete(const std::string& hash)
{
    auto it = pendingScrapes_.find(hash);
    if (it == pendingScrapes_.end())
        return;

    if (it->second.pendingCount > 0)
        return; // still waiting for some strategies

    // All strategies complete -- fold the results into a single JSON object.
    // Only the freshly scraped keys are emitted; the listener merges them
    // into the stored torrent.
    librats::Json info = librats::Json::object();
    librats::Json trackers = librats::Json::array();

    for (const TrackerSiteInfo& result : it->second.results) {
        // Add tracker to the list (deduplicated).
        bool alreadyListed = false;
        for (const librats::Json& t : trackers) {
            if (t.is_string() && t.get<std::string>() == result.trackerName) {
                alreadyListed = true;
                break;
            }
        }
        if (!alreadyListed)
            trackers.push_back(result.trackerName);

        // Merge shared fields -- first found wins for poster / description.
        if (info.value("poster", std::string()).empty() && !result.poster.empty())
            info["poster"] = result.poster;
        if (info.value("description", std::string()).empty() && !result.description.empty())
            info["description"] = result.description;

        // Tracker-specific payloads.
        if (result.trackerName == "rutracker") {
            if (result.threadId > 0)
                info["rutrackerThreadId"] = result.threadId;
            if (!result.contentCategory.empty())
                info["contentCategory"] = result.contentCategory;
            if (!result.name.empty() && !info.contains("trackerName"))
                info["trackerName"] = result.name;
        } else if (result.trackerName == "nyaa") {
            if (result.threadId > 0)
                info["nyaaThreadId"] = result.threadId;
        }
    }

    const bool any = !trackers.empty();
    if (any)
        info["trackers"] = trackers;

    pendingScrapes_.erase(it);

    // This hash is done -- free its concurrency slot and let queued scrapes start.
    if (activeRequests_ > 0)
        --activeRequests_;
    processQueue();

    // Only report when at least one tracker produced data.
    if (any && scraped_)
        scraped_(hash, info);
}

// ============================================================================
// HTML helpers
// ============================================================================

std::string TrackerSiteScraper::truncateDescription(const std::string& text)
{
    if (text.size() > static_cast<size_t>(kMaxDescriptionLength))
        return text.substr(0, kMaxDescriptionLength) + "...";
    return text;
}

std::string TrackerSiteScraper::stripHtml(const std::string& html)
{
    std::string text = html;

    // Replace <br>, <br/>, <br /> with newlines.
    text = replaceAllRegex(text, R"(<br\s*/?>)", "\n", PCRE2_CASELESS);

    // Replace block-level tags with newlines.
    text = replaceAllRegex(text, "</(?:p|div|li|tr|h[1-6])>", "\n", PCRE2_CASELESS);

    // Remove all remaining HTML tags.
    text = replaceAllRegex(text, "<[^>]*>", "");

    // Decode common HTML entities (plain substring replace, not regex --
    // matches Qt's literal QString::replace calls here).
    auto replaceLiteral = [](std::string& s, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceLiteral(text, "&amp;", "&");
    replaceLiteral(text, "&lt;", "<");
    replaceLiteral(text, "&gt;", ">");
    replaceLiteral(text, "&quot;", "\"");
    replaceLiteral(text, "&apos;", "'");
    replaceLiteral(text, "&#39;", "'");
    replaceLiteral(text, "&nbsp;", " ");
    replaceLiteral(text, "&#160;", " ");

    // Decode numeric entities.
    text = decodeNumericEntities(text);

    // Collapse runs of blank lines to at most two.
    text = replaceAllRegex(text, "\\n{3,}", "\n\n");

    // Trim whitespace from each line.
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        lines.push_back(trim(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start)));
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    text = join(lines, "\n");

    return trim(text);
}

std::string TrackerSiteScraper::decodeWindows1251(const std::string& data)
{
    // Windows-1251 to Unicode mapping for bytes 0x80-0xFF.
    static const std::uint16_t kTable[128] = {
        // 0x80-0x8F
        0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A,
        0x040C, 0x040B, 0x040F,
        // 0x90-0x9F
        0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0098, 0x2122, 0x0459, 0x203A, 0x045A,
        0x045C, 0x045B, 0x045F,
        // 0xA0-0xAF
        0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC,
        0x00AD, 0x00AE, 0x0407,
        // 0xB0-0xBF
        0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458,
        0x0405, 0x0455, 0x0457,
        // 0xC0-0xCF: А-П (U+0410-U+041F)
        0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C,
        0x041D, 0x041E, 0x041F,
        // 0xD0-0xDF: Р-Я (U+0420-U+042F)
        0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042A, 0x042B, 0x042C,
        0x042D, 0x042E, 0x042F,
        // 0xE0-0xEF: а-п (U+0430-U+043F)
        0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C,
        0x043D, 0x043E, 0x043F,
        // 0xF0-0xFF: р-я (U+0440-U+044F)
        0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, 0x0448, 0x0449, 0x044A, 0x044B, 0x044C,
        0x044D, 0x044E, 0x044F,
    };

    std::string result;
    result.reserve(data.size());
    for (unsigned char ch : data) {
        if (ch < 0x80)
            result.push_back(static_cast<char>(ch));
        else
            result += utf8Encode(kTable[ch - 0x80]);
    }
    return result;
}

} // namespace ratsn::engine
