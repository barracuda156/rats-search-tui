#include "domain/filter_policy.h"

#include "domain/content.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <algorithm>
#include <cctype>
#include <vector>

namespace ratsn::domain {

// Owns the compiled pattern; kept out of the header so callers don't need
// <pcre2.h>, matching GroongaIndex's opaque-pointer style for grn_ctx/grn_obj.
struct FilterPolicy::CompiledRegex {
    pcre2_code* code = nullptr;
    ~CompiledRegex()
    {
        if (code)
            pcre2_code_free(code);
    }
};

namespace {

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

bool equalsIgnoreCase(const std::string& a, const std::string& b)
{
    return toLower(a) == toLower(b);
}

std::vector<std::string> splitCsv(const std::string& s)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t comma = s.find(',', start);
        const std::string piece = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!piece.empty())
            out.push_back(piece);
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return out;
}

bool regexMatches(const pcre2_code* code, const std::string& subject)
{
    if (!code)
        return false;
    pcre2_match_data* match = pcre2_match_data_create_from_pattern(code, nullptr);
    const int rc = pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(subject.c_str()), subject.size(), 0, 0, match, nullptr);
    pcre2_match_data_free(match);
    return rc >= 0;
}

} // namespace

FilterPolicy::FilterPolicy()
    : FilterPolicy(FilterSettings {})
{
}

FilterPolicy::FilterPolicy(FilterSettings settings)
    : settings_(std::move(settings))
{
    compileNamingRegex();
}

FilterPolicy::~FilterPolicy() = default;
FilterPolicy::FilterPolicy(FilterPolicy&&) noexcept = default;
FilterPolicy& FilterPolicy::operator=(FilterPolicy&&) noexcept = default;

void FilterPolicy::setSettings(FilterSettings settings)
{
    settings_ = std::move(settings);
    compileNamingRegex();
}

void FilterPolicy::compileNamingRegex()
{
    namingRegex_ = std::make_unique<CompiledRegex>();
    if (settings_.namingRegExp.empty())
        return;

    int errorCode = 0;
    PCRE2_SIZE errorOffset = 0;
    // Left null on a compile error; checkNamingRegExp then treats it the
    // same as "no filter configured", matching QRegularExpression::isValid().
    namingRegex_->code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(settings_.namingRegExp.c_str()),
        PCRE2_ZERO_TERMINATED, PCRE2_CASELESS, &errorCode, &errorOffset, nullptr);
}

std::string FilterPolicy::rejectionReason(const Torrent& t) const
{
    if (std::string r = checkFileCount(t); !r.empty())
        return r;
    if (std::string r = checkSize(t); !r.empty())
        return r;
    if (std::string r = checkAdult(t); !r.empty())
        return r;
    if (std::string r = checkNamingRegExp(t); !r.empty())
        return r;
    if (std::string r = checkContentType(t); !r.empty())
        return r;
    return {};
}

std::string FilterPolicy::checkFileCount(const Torrent& t) const
{
    if (settings_.maxFiles > 0 && t.files > settings_.maxFiles)
        return "Too many files: " + std::to_string(t.files) + " > " + std::to_string(settings_.maxFiles);
    return {};
}

std::string FilterPolicy::checkSize(const Torrent& t) const
{
    if (settings_.sizeMin > 0 && t.size < settings_.sizeMin)
        return "Size too small: " + std::to_string(t.size) + " < " + std::to_string(settings_.sizeMin);
    if (settings_.sizeMax > 0 && t.size > settings_.sizeMax)
        return "Size too large: " + std::to_string(t.size) + " > " + std::to_string(settings_.sizeMax);
    return {};
}

std::string FilterPolicy::checkAdult(const Torrent& t) const
{
    if (!settings_.adultFilter)
        return {};

    static const std::vector<std::string> keywords = { "xxx", "porn", "sex", "adult", "18+", "nsfw" };
    const std::string nameLower = toLower(t.name);
    for (const std::string& keyword : keywords) {
        if (nameLower.find(keyword) != std::string::npos)
            return "Adult content detected: " + keyword;
    }
    if (t.contentCategory == ContentCategory::XXX)
        return "Adult content category";
    return {};
}

std::string FilterPolicy::checkNamingRegExp(const Torrent& t) const
{
    if (settings_.namingRegExp.empty() || !namingRegex_ || !namingRegex_->code)
        return {};

    const bool matches = regexMatches(namingRegex_->code, t.name);
    if (settings_.namingRegExpNegative) {
        if (matches)
            return "Name matches blocked pattern: " + settings_.namingRegExp;
    } else if (!matches) {
        return "Name doesn't match required pattern: " + settings_.namingRegExp;
    }
    return {};
}

std::string FilterPolicy::checkContentType(const Torrent& t) const
{
    const std::string& filter = settings_.contentTypeFilter;
    if (filter.empty() || equalsIgnoreCase(filter, "all"))
        return {};

    const std::vector<std::string> allowed = splitCsv(filter);
    if (allowed.empty())
        return {};

    const std::string typeName = toString(t.contentType);
    for (const std::string& raw : allowed) {
        const std::string type = trim(raw);
        // "application" is a UI umbrella token spanning Software + Games
        // (mirrors data::TorrentRepository::contentTypeFilter). "disc" has no
        // matching ContentType and is a legacy no-op token.
        if (equalsIgnoreCase(type, "application")) {
            if (t.contentType == ContentType::Software || t.contentType == ContentType::Games)
                return {};
            continue;
        }
        if (equalsIgnoreCase(typeName, type))
            return {};
    }
    return "Content type not allowed: " + typeName;
}

} // namespace ratsn::domain
