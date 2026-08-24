#pragma once

#include "domain/torrent.h"

#include <cstdint>
#include <memory>
#include <string>

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

    // Empty string = accepted; otherwise a human-readable rejection reason.
    std::string rejectionReason(const Torrent& torrent) const;
    bool accepts(const Torrent& torrent) const { return rejectionReason(torrent).empty(); }

private:
    std::string checkFileCount(const Torrent& t) const;
    std::string checkSize(const Torrent& t) const;
    std::string checkAdult(const Torrent& t) const;
    std::string checkNamingRegExp(const Torrent& t) const;
    std::string checkContentType(const Torrent& t) const;

    // Recompile the naming regex (PCRE2) from settings_; called whenever
    // settings change, so the hot per-torrent path reuses one compiled
    // pattern.
    void compileNamingRegex();

    FilterSettings settings_;

    struct CompiledRegex;
    std::unique_ptr<CompiledRegex> namingRegex_;
};

} // namespace ratsn::domain
