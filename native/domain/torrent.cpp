#include "domain/torrent.h"

#include <cctype>
#include <cstdio>

namespace ratsn::domain {

namespace {
constexpr size_t kHashLength = 40;

// RFC 3986 percent-encoding for a magnet URI's `dn` parameter.
std::string percentEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}
} // namespace

bool Torrent::isValid() const
{
    if (hash.size() != kHashLength)
        return false;
    for (char c : hash) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    }
    return true;
}

std::string Torrent::magnetLink() const
{
    return "magnet:?xt=urn:btih:" + hash + "&dn=" + percentEncode(name);
}

} // namespace ratsn::domain
