#pragma once

#include "domain/torrent.h"
#include "librats/util/json.h"

// The single source of truth for turning torrents into wire JSON and back.
// Field names are copied verbatim from src/domain/torrent_codec.cpp
// (docs/DESIGN-native.md §6) — this is a transliteration, not a redesign; the
// P2P peer API (M4) serialises through this exact codec so the wire format
// cannot drift from the existing Qt app.
namespace ratsn::domain::codec {

struct ToJsonOptions {
    bool includeFiles = false; // embed the "files_list" array (path/size objects)
    bool includeInfo = true; // embed the scraped "info" object when non-empty
};

librats::Json toJson(const Torrent& torrent, ToJsonOptions options = {});
librats::Json toJson(const SearchHit& hit, ToJsonOptions options = {});

// Tolerant parser: accepts either "hash" or the legacy "info_hash" key, parses
// content type/category from their string names, and reads an embedded
// "files_list" (or legacy "filesList") array if present.
Torrent torrentFromJson(const librats::Json& obj);
SearchHit searchHitFromJson(const librats::Json& obj);

librats::Json filesToJson(const std::vector<File>& files);
std::vector<File> filesFromJson(const librats::Json& array);

} // namespace ratsn::domain::codec
