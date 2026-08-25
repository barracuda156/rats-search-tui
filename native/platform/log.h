#pragma once

#include <sstream>
#include <string>

// A tiny, swappable diagnostic sink for ratsn's own engine components
// (Crawler, Indexer, PeerApi, PeerRegistry, Replication). Deliberately
// separate from librats::Logger (a different library's own singleton,
// already redirected to a file during the TUI session -- see tui/app.cpp)
// and, more importantly, from raw std::cout/std::cerr: FTXUI's own screen
// rendering writes through std::cout too, so a blanket redirect of either
// stream would silently swallow the TUI right along with the log spam --
// the exact bug tui/app.cpp already fixed once for librats::Logger. This
// gives ratsn's own diagnostics the same kind of dedicated, redirectable
// sink instead of writing to std::cout/std::cerr directly.
namespace ratsn::platform {

namespace detail {

// Buffers everything streamed into it, then flushes as one lock-protected
// write when the temporary is destroyed at the end of the full expression
// -- so a line built with several `<<` calls (as every call site here does)
// still lands as a single atomic write, matching the one-line-per-call-site
// discipline diagnostics were already written in. Needed because these
// calls come from more than one thread (some librats callbacks are
// documented as firing on a worker thread; see e.g. crawler.cpp's
// fetchMetadata).
class LogLine {
public:
    LogLine() = default;
    ~LogLine();

    LogLine(const LogLine&) = delete;
    LogLine& operator=(const LogLine&) = delete;

    template <typename T>
    LogLine& operator<<(const T& value)
    {
        buf_ << value;
        return *this;
    }

private:
    std::ostringstream buf_;
};

} // namespace detail

// ratsn::platform::log() << "Crawler: announce " << hash << "\n"; -- a
// near-drop-in replacement for std::cout/std::cerr at every diagnostic call
// site (just add the parentheses). Writes to stdout until
// enableFileLogging() is called.
inline detail::LogLine log() { return {}; }

// Redirects subsequent log() lines to `path` (truncating any existing
// file) instead of stdout. Returns false (and leaves logging on stdout) if
// the file can't be opened. Call once when entering the TUI.
bool enableFileLogging(const std::string& path);

// Reverts to stdout. Call when leaving the TUI.
void disableFileLogging();

} // namespace ratsn::platform
