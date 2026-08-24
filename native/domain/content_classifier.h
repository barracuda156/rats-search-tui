#pragma once

#include "domain/content.h"
#include "domain/torrent.h"

#include <string>
#include <vector>

namespace ratsn::domain {

// Determines a torrent's content type/category from its name and file list.
//
// Detection is weighted-by-size file-type voting, plus adult-content word
// blocking for video/pictures/archive. The extension->type table and the
// bad-word lists are compiled in (native/CMakeLists.txt embeds
// resources/content/{extensions,badwords}.json as raw string literals; no
// Qt resource bundle here) instead of being loaded from disk.
struct Classification {
    ContentType type = ContentType::Unknown;
    ContentCategory category = ContentCategory::Unknown;
};

class ContentClassifier {
public:
    // Classify from a name and file list. Pure -- no side effects.
    static Classification classify(const std::string& name, const std::vector<File>& files);

    // Convenience: classify and write the result back onto the torrent.
    static void classify(Torrent& torrent);

    // The size-weighted file-type vote in isolation (exposed for testing).
    static ContentType detectTypeFromFiles(const std::vector<File>& files);
};

} // namespace ratsn::domain
