#pragma once

#include "domain/content.h"
#include "librats/util/json.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ratsn::domain {

// A single file inside a torrent.
struct File {
    std::string path;
    int64_t size = 0;
};

// A torrent as indexed by the search engine — port of src/domain/torrent.h to
// std types. `added`/`trackersChecked` are milliseconds since the Unix epoch,
// matching the wire codec (docs/DESIGN-native.md §6), not the seconds used by
// the Qt app's SQL layer internally.
struct Torrent {
    std::string hash; // 40-char lower-case hex info-hash
    std::string name;
    int64_t size = 0;
    int files = 0;
    int pieceLength = 0;
    int64_t added = 0;
    ContentType contentType = ContentType::Unknown;
    ContentCategory contentCategory = ContentCategory::Unknown;
    int seeders = 0;
    int leechers = 0;
    int completed = 0;
    int64_t trackersChecked = 0; // 0 = never checked
    int good = 0; // up-votes
    int bad = 0; // down-votes
    librats::Json info; // scraped extras: poster, description, tracker payloads
    std::vector<File> fileList;

    bool isValid() const;
    std::string magnetLink() const;
};

// A search result: a torrent plus the metadata specific to *how* it was
// found. Kept separate from Torrent so the entity stays clean and the search
// layer owns its own concerns (file-match highlighting, remote provenance).
struct SearchHit {
    Torrent torrent;
    bool fromFileMatch = false; // matched on a file path rather than the name
    std::vector<std::string> matchingPaths; // highlighted file-path snippets
    std::string sourcePeerId; // non-empty if this hit came from a remote peer
    bool remote = false;
    // Seconds since the Unix epoch this torrent entered the feed (docs/
    // M7-PLAN.md item 7); 0 outside a feed listing.
    int64_t feedDate = 0;
};

} // namespace ratsn::domain
