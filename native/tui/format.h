#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

// Small display-formatting helpers shared by search_tab and status_bar. Not
// part of docs/DESIGN-native.md's module breakdown (which lists whole tabs,
// not utilities) -- kept header-only since both callers need it and it's a
// handful of lines.
namespace ratsn::tui {

// Content-type toggle labels/values shared by the Search filter row and the
// Top tab (docs/M5-PLAN.md items 2/5 -- "same labels as the filter panel").
// Index-parallel with domain::FilterSettings::contentTypeFilter's CSV tokens
// ("application" is the Software+Games umbrella; "" means "all").
inline const std::vector<std::string> kContentTypeLabels
    = { "all", "video", "audio", "pictures", "books", "application", "archive" };
inline const std::vector<std::string> kContentTypeValues
    = { "", "video", "audio", "pictures", "books", "application", "archive" };

inline std::string humanSize(int64_t bytes)
{
    static const char* kUnits[] = { "B", "K", "M", "G", "T", "P" };
    constexpr size_t kUnitCount = sizeof(kUnits) / sizeof(kUnits[0]);
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < kUnitCount) {
        value /= 1024.0;
        ++unit;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), unit == 0 ? "%.0f%s" : "%.1f%s", value, kUnits[unit]);
    return buf;
}

// Parses a size with an optional unit suffix ("700M", "1.5G", "512", ...) --
// K/M/G/T are binary (1024-based), matching humanSize's units above. Empty,
// unparsable, or negative input yields 0 ("no limit"), the same sentinel
// SearchQuery::sizeMin/sizeMax already use.
inline int64_t parseSize(const std::string& text)
{
    if (text.empty())
        return 0;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || value < 0)
        return 0;
    int64_t multiplier = 1;
    switch (std::toupper(static_cast<unsigned char>(*end))) {
    case 'K':
        multiplier = 1024LL;
        break;
    case 'M':
        multiplier = 1024LL * 1024;
        break;
    case 'G':
        multiplier = 1024LL * 1024 * 1024;
        break;
    case 'T':
        multiplier = 1024LL * 1024 * 1024 * 1024;
        break;
    default:
        break;
    }
    return static_cast<int64_t>(value * static_cast<double>(multiplier));
}

// `epochMs` is milliseconds since the Unix epoch (domain::Torrent::added's
// unit, per docs/DESIGN-native.md §6).
inline std::string humanDate(int64_t epochMs)
{
    if (epochMs <= 0)
        return "-";
    const std::time_t t = static_cast<std::time_t>(epochMs / 1000);
    std::tm tmv {};
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return buf;
}

} // namespace ratsn::tui
