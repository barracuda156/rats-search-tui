#pragma once

#include "domain/torrent.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ratsn::domain {

// Plain settings that drive the filter. Kept independent of
// ratsn::platform::Config/FilterConfig (the caller translates) so the policy
// stays pure and trivially testable, matching src/services/filter_policy.h.
struct FilterSettings {
    int maxFiles = 0; // 0 = no limit
    int64_t sizeMin = 0;
    int64_t sizeMax = 0;
    bool adultFilter = false;
    std::string namingRegExp;
    bool namingRegExpNegative = false;
    std::string contentTypeFilter; // empty or "all" = accept all types; otherwise CSV
    // Native extension (docs/M5-PLAN.md item 3), no Qt equivalent: tracker
    // identity comes only from info["trackers"], which DHT-crawled records
    // never carry (see the M5-PLAN item 3 caveat) -- only records that
    // passed a Qt peer with tracker-site scraping have it.
    std::vector<std::string> trackerAllow; // empty = no allow-list restriction
    std::vector<std::string> trackerDeny; // empty = no deny-list restriction
    bool trackerRequireKnown = false; // reject torrents with no tracker identity at all
};

// Decides whether a torrent is allowed into the index -- one method per rule.
class FilterPolicy {
public:
    FilterPolicy();
    explicit FilterPolicy(FilterSettings settings);
    ~FilterPolicy();

    FilterPolicy(FilterPolicy&&) noexcept;
    FilterPolicy& operator=(FilterPolicy&&) noexcept;
    FilterPolicy(const FilterPolicy&) = delete;
    FilterPolicy& operator=(const FilterPolicy&) = delete;

    void setSettings(FilterSettings settings);
    const FilterSettings& settings() const { return settings_; }

    // An unparsable pattern is silently treated as "no filter" by
    // checkNamingRegExp below (matching QRegularExpression::isValid()'s
    // effect in the Qt app) -- which reads as "the filter does nothing"
    // rather than failing loudly. `ratsn cleanup` (docs/M5-PLAN.md item 8,
    // porting the Qt REST handler's same pre-check) uses this to reject an
    // invalid pattern up front instead.
    static bool isValidNamingRegExp(const std::string& pattern, std::string* error = nullptr);

    // Empty string = accepted; otherwise a human-readable rejection reason.
    std::string rejectionReason(const Torrent& torrent) const;
    bool accepts(const Torrent& torrent) const { return rejectionReason(torrent).empty(); }

private:
    std::string checkFileCount(const Torrent& t) const;
    std::string checkSize(const Torrent& t) const;
    std::string checkAdult(const Torrent& t) const;
    std::string checkNamingRegExp(const Torrent& t) const;
    std::string checkContentType(const Torrent& t) const;
    std::string checkTrackerPolicy(const Torrent& t) const;

    // Recompile the naming regex (PCRE2) from settings_; called whenever
    // settings change, so the hot per-torrent path reuses one compiled
    // pattern.
    void compileNamingRegex();

    FilterSettings settings_;

    struct CompiledRegex;
    std::unique_ptr<CompiledRegex> namingRegex_;
};

} // namespace ratsn::domain
