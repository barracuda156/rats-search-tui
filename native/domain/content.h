#pragma once

#include <string>

namespace ratsn::domain {

// Numeric values are the ids stored in the search index and on the wire
// (docs/DESIGN-native.md §6); port of src/domain/content.h — never renumber.
enum class ContentType {
    Unknown = 0,
    Video = 1,
    Audio = 2,
    Books = 3,
    Pictures = 4,
    Software = 5,
    Games = 6,
    Archive = 7,
    Bad = 100,
};

enum class ContentCategory {
    Unknown = 0,
    Movie = 1,
    Series = 2,
    Documentary = 3,
    Anime = 4,
    Music = 5,
    Ebook = 6,
    Comics = 7,
    Software = 8,
    Game = 9,
    XXX = 100,
};

// Canonical wire/JSON names (lower-case, e.g. "video", "movie"). Unknown/empty
// maps to the Unknown enum.
std::string toString(ContentType type);
std::string toString(ContentCategory category);
ContentType contentTypeFromString(const std::string& s);
ContentCategory contentCategoryFromString(const std::string& s);

inline int toId(ContentType type)
{
    return static_cast<int>(type);
}
inline int toId(ContentCategory category)
{
    return static_cast<int>(category);
}
ContentType contentTypeFromId(int id);
ContentCategory contentCategoryFromId(int id);

} // namespace ratsn::domain
