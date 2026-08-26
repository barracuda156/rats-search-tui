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

// `epochMs` is milliseconds since the Unix epoch (domain::Torrent::
// trackersChecked's unit). Used for the details pane's "seeders: N (checked
// 3m ago)" display (docs/M8-PLAN.md item 7).
inline std::string humanAge(int64_t epochMs)
{
    if (epochMs <= 0)
        return "never";
    const int64_t nowMs = static_cast<int64_t>(std::time(nullptr)) * 1000;
    int64_t deltaSec = (nowMs - epochMs) / 1000;
    if (deltaSec < 0)
        deltaSec = 0;
    if (deltaSec < 60)
        return std::to_string(deltaSec) + "s ago";
    if (deltaSec < 3600)
        return std::to_string(deltaSec / 60) + "m ago";
    if (deltaSec < 86400)
        return std::to_string(deltaSec / 3600) + "h ago";
    return std::to_string(deltaSec / 86400) + "d ago";
}

// Greedy word-wrap for the details pane's scraped description (docs/
// M8-PLAN.md item 7). Not FTXUI's paragraph() -- that pulls in a layout
// dependency this header doesn't otherwise have, for a one-off need here.
// Preserves embedded newlines as paragraph breaks.
inline std::vector<std::string> wrapText(const std::string& text, size_t width)
{
    std::vector<std::string> lines;
    size_t paraStart = 0;
    while (paraStart <= text.size()) {
        const size_t nl = text.find('\n', paraStart);
        const std::string para = text.substr(paraStart, nl == std::string::npos ? std::string::npos : nl - paraStart);

        std::string current;
        size_t pos = 0;
        while (pos < para.size()) {
            const size_t spaceEnd = para.find(' ', pos);
            const std::string word = para.substr(pos, spaceEnd == std::string::npos ? std::string::npos : spaceEnd - pos);
            if (!current.empty() && current.size() + 1 + word.size() > width) {
                lines.push_back(current);
                current.clear();
            }
            if (!current.empty())
                current += ' ';
            current += word;
            if (spaceEnd == std::string::npos)
                break;
            pos = spaceEnd + 1;
        }
        lines.push_back(current);

        if (nl == std::string::npos)
            break;
        paraStart = nl + 1;
    }
    return lines;
}

} // namespace ratsn::tui
