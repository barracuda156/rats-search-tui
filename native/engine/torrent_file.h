#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace librats {
class Bittorrent;
namespace bittorrent {
class TorrentInfo;
}
}

// Save-.torrent support for the TUI's 't' key (docs/M5-PLAN.md item 6):
// splices announce/announce-list keys around a fetched torrent's raw
// info-dict bytes to produce a complete .torrent byte stream. Port of
// src/net/torrent_engine.cpp's assembleTorrentFile/buildTorrentBytes to std
// types -- TUI-only (a CLI variant would need the whole node/DHT started for
// one fetch, out of scope per M5-PLAN.md).
namespace ratsn::engine {

// Never decode+re-encode the info dict: that would re-sort its keys and
// change the infohash, so the raw bytes are spliced in verbatim. Keys are
// emitted in bencode-sorted order (announce < announce-list < info).
std::vector<uint8_t> assembleTorrentFile(const librats::bittorrent::TorrentInfo& info);

// Exactly one of (non-empty bytes) / (non-empty error) on completion. Calls
// `bittorrent.get_torrent_metadata` -- the same BEP 9 fetch
// Crawler::fetchMetadata uses -- and fires `callback` on a librats worker
// thread, per that call's documented contract; the caller must marshal
// before touching any engine/UI state.
using FetchTorrentFileCallback = std::function<void(std::vector<uint8_t> bytes, std::string error)>;
void fetchTorrentFile(librats::Bittorrent& bittorrent, const std::string& infoHashHex, FetchTorrentFileCallback callback);

} // namespace ratsn::engine
