#include "domain/content.h"

#include <algorithm>
#include <cctype>

namespace ratsn::domain {

namespace {
std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}
} // namespace

std::string toString(ContentType type)
{
    switch (type) {
    case ContentType::Video:
        return "video";
    case ContentType::Audio:
        return "audio";
    case ContentType::Books:
        return "books";
    case ContentType::Pictures:
        return "pictures";
    case ContentType::Software:
        return "software";
    case ContentType::Games:
        return "games";
    case ContentType::Archive:
        return "archive";
    case ContentType::Bad:
        return "bad";
    case ContentType::Unknown:
        break;
    }
    return {};
}

std::string toString(ContentCategory category)
{
    switch (category) {
    case ContentCategory::Movie:
        return "movie";
    case ContentCategory::Series:
        return "series";
    case ContentCategory::Documentary:
        return "documentary";
    case ContentCategory::Anime:
        return "anime";
    case ContentCategory::Music:
        return "music";
    case ContentCategory::Ebook:
        return "ebook";
    case ContentCategory::Comics:
        return "comics";
    case ContentCategory::Software:
        return "software";
    case ContentCategory::Game:
        return "game";
    case ContentCategory::XXX:
        return "xxx";
    case ContentCategory::Unknown:
        break;
    }
    return {};
}

ContentType contentTypeFromString(const std::string& s)
{
    const std::string v = toLower(s);
    if (v == "video")
        return ContentType::Video;
    if (v == "audio")
        return ContentType::Audio;
    if (v == "books")
        return ContentType::Books;
    if (v == "pictures")
        return ContentType::Pictures;
    if (v == "software")
        return ContentType::Software;
    if (v == "games")
        return ContentType::Games;
    if (v == "archive")
        return ContentType::Archive;
    if (v == "bad")
        return ContentType::Bad;
    return ContentType::Unknown;
}

ContentCategory contentCategoryFromString(const std::string& s)
{
    const std::string v = toLower(s);
    if (v == "movie")
        return ContentCategory::Movie;
    if (v == "series")
        return ContentCategory::Series;
    if (v == "documentary")
        return ContentCategory::Documentary;
    if (v == "anime")
        return ContentCategory::Anime;
    if (v == "music")
        return ContentCategory::Music;
    if (v == "ebook")
        return ContentCategory::Ebook;
    if (v == "comics")
        return ContentCategory::Comics;
    if (v == "software")
        return ContentCategory::Software;
    if (v == "game")
        return ContentCategory::Game;
    if (v == "xxx")
        return ContentCategory::XXX;
    return ContentCategory::Unknown;
}

ContentType contentTypeFromId(int id)
{
    switch (id) {
    case 1:
        return ContentType::Video;
    case 2:
        return ContentType::Audio;
    case 3:
        return ContentType::Books;
    case 4:
        return ContentType::Pictures;
    case 5:
        return ContentType::Software;
    case 6:
        return ContentType::Games;
    case 7:
        return ContentType::Archive;
    case 100:
        return ContentType::Bad;
    default:
        return ContentType::Unknown;
    }
}

ContentCategory contentCategoryFromId(int id)
{
    switch (id) {
    case 1:
        return ContentCategory::Movie;
    case 2:
        return ContentCategory::Series;
    case 3:
        return ContentCategory::Documentary;
    case 4:
        return ContentCategory::Anime;
    case 5:
        return ContentCategory::Music;
    case 6:
        return ContentCategory::Ebook;
    case 7:
        return ContentCategory::Comics;
    case 8:
        return ContentCategory::Software;
    case 9:
        return ContentCategory::Game;
    case 100:
        return ContentCategory::XXX;
    default:
        return ContentCategory::Unknown;
    }
}

} // namespace ratsn::domain
