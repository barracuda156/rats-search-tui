#include "engine/torrent_file.h"

#include "librats/bittorrent/torrent_info.h"
#include "librats/subsystems/bittorrent.h"

namespace ratsn::engine {

namespace {
namespace bt = librats::bittorrent;

std::vector<uint8_t> buildTorrentBytes(const std::vector<uint8_t>& infoDict, const std::vector<std::string>& trackers)
{
    std::vector<uint8_t> out;
    out.reserve(infoDict.size() + 256);
    auto appendStr = [&out](const std::string& s) { out.insert(out.end(), s.begin(), s.end()); };
    auto appendBytes = [&out](char c) { out.push_back(static_cast<uint8_t>(c)); };

    appendBytes('d');
    if (!trackers.empty()) {
        appendStr("8:announce");
        appendStr(std::to_string(trackers.front().size()) + ":");
        appendStr(trackers.front());
    }
    if (trackers.size() > 1) {
        appendStr("13:announce-list");
        appendBytes('l');
        for (const std::string& t : trackers) {
            appendBytes('l');
            appendStr(std::to_string(t.size()) + ":");
            appendStr(t);
            appendBytes('e');
        }
        appendBytes('e');
    }
    appendStr("4:info");
    out.insert(out.end(), infoDict.begin(), infoDict.end());
    appendBytes('e');
    return out;
}

} // namespace

std::vector<uint8_t> assembleTorrentFile(const bt::TorrentInfo& info)
{
    return buildTorrentBytes(info.info_dict_bytes(), info.all_trackers());
}

void fetchTorrentFile(librats::Bittorrent& bittorrent, const std::string& infoHashHex, FetchTorrentFileCallback callback)
{
    bittorrent.get_torrent_metadata(
        infoHashHex,
        [callback](const bt::TorrentInfo& info, bool success, const std::string& error) {
            if (!success || !info.is_valid()) {
                callback({}, error.empty() ? "metadata fetch failed" : error);
                return;
            }
            callback(assembleTorrentFile(info), {});
        },
        kFetchTorrentFileTimeoutMs);
}

} // namespace ratsn::engine
