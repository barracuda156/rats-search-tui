#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

// Small display-formatting helpers shared by search_tab and status_bar. Not
// part of docs/DESIGN-native.md's module breakdown (which lists whole tabs,
// not utilities) -- kept header-only since both callers need it and it's a
// handful of lines.
namespace ratsn::tui {

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
