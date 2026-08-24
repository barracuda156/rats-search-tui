#include "domain/content_classifier.h"

#include "classifier_data.h"
#include "librats/util/json.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

// Data notes
// ----------
// The extension->type table and bad-word lists are embedded at configure
// time from resources/content/*.json (native/CMakeLists.txt) into
// classifier_data.h.
//
// Ten extensions are claimed by more than one content type; extensions.json
// keeps unique keys, so the winner is baked in: mod->audio, mng->pictures,
// pkg->games, md->games, iso->archive, cue->archive, img->archive, cab->archive,
// bin->archive, ccd->archive. Bad words are deduped (block: 1710, veryBad: 144).

namespace ratsn::domain {

namespace {

struct ClassifierData {
    std::unordered_map<std::string, std::string> extensionToType;
    std::unordered_set<std::string> blockWords;
    std::unordered_set<std::string> veryBadWords;
};

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Delimiter set matching the legacy
// name.split(/[`~!@#$%^&*()\]\[{}.,+?/\\;:\-_' "|]/). Hand-rolled rather than
// pulling PCRE2 in for one fixed, never-configurable pattern (filter_policy's
// naming-regex filter is the only thing that needs PCRE2).
bool isNameDelimiter(unsigned char c)
{
    static constexpr const char* kDelimiters = "`~!@#$%^&*()][{}.,+?/\\;:-_' \"|";
    for (const char* p = kDelimiters; *p; ++p) {
        if (static_cast<unsigned char>(*p) == c)
            return true;
    }
    return false;
}

std::vector<std::string> splitOnDelimiters(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (isNameDelimiter(static_cast<unsigned char>(ch))) {
            if (!cur.empty()) {
                out.push_back(std::move(cur));
                cur.clear();
            }
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty())
        out.push_back(std::move(cur));
    return out;
}

const ClassifierData& data()
{
    static const ClassifierData d = [] {
        ClassifierData out;

        const librats::Json exts = librats::Json::parse(embedded::kExtensionsJson, nullptr, false);
        if (exts.is_object()) {
            // Object::storage is std::vector<std::pair<std::string, Json>>, so
            // this decomposes cleanly. Json::items()'s Iterator::operator*()
            // returns just the value (key() is a separate call on the
            // iterator) despite its own header comment showing [key, val]
            // structured bindings -- that usage fails to compile (Json isn't
            // itself decomposable: private members, anonymous union).
            for (const auto& [key, value] : exts.as_object()) {
                if (value.is_string())
                    out.extensionToType.emplace(key, value.get<std::string>());
            }
        }

        const librats::Json words = librats::Json::parse(embedded::kBadwordsJson, nullptr, false);
        if (words.is_object()) {
            if (const librats::Json* block = words.as_object().find("blockWords"); block && block->is_array()) {
                for (const librats::Json& v : *block)
                    out.blockWords.insert(v.get<std::string>());
            }
            if (const librats::Json* veryBad = words.as_object().find("veryBadWords"); veryBad && veryBad->is_array()) {
                for (const librats::Json& v : *veryBad)
                    out.veryBadWords.insert(v.get<std::string>());
            }
        }

        return out;
    }();
    return d;
}

// Map a file path to a content-type string ("video"/"audio"/...) or empty.
std::string detectFileType(const std::string& filePath)
{
    std::string name = filePath;
    if (const auto slash = name.find_last_of('/'); slash != std::string::npos)
        name = name.substr(slash + 1);
    if (const auto backslash = name.find_last_of('\\'); backslash != std::string::npos)
        name = name.substr(backslash + 1);
    if (name.empty())
        return {};

    const auto dotPos = name.find_last_of('.');
    if (dotPos == std::string::npos || dotPos == name.size() - 1)
        return {};

    const auto it = data().extensionToType.find(toLower(name.substr(dotPos + 1)));
    return it != data().extensionToType.end() ? it->second : std::string();
}

// Apply the adult-content word rules to a single name; may raise `type` to Bad
// (terminal) or `category` to XXX (non-terminal). Returns true once Bad is set.
bool applyBadWords(const std::string& name, ContentType& type, ContentCategory& category)
{
    const ClassifierData& d = data();
    for (const std::string& word : splitOnDelimiters(toLower(name))) {
        if (d.veryBadWords.count(word) > 0) {
            type = ContentType::Bad;
            return true;
        }
        if (d.blockWords.count(word) > 0)
            category = ContentCategory::XXX;
    }
    return false;
}

} // namespace

ContentType ContentClassifier::detectTypeFromFiles(const std::vector<File>& files)
{
    if (files.empty())
        return ContentType::Unknown;

    int64_t totalSize = 0;
    for (const File& f : files)
        totalSize += f.size;

    // Weighted vote: each file adds (file.size / totalSize) to its type. When all
    // sizes are zero, fall back to equal weight-per-file (1 / fileCount).
    std::unordered_map<std::string, double> priority;
    for (const File& f : files) {
        const std::string type = detectFileType(f.path);
        if (type.empty())
            continue;
        const double weight
            = totalSize > 0 ? static_cast<double>(f.size) / static_cast<double>(totalSize) : 1.0 / files.size();
        priority[type] += weight;
    }

    std::string bestType;
    double bestWeight = 0.0;
    for (const auto& [type, weight] : priority) {
        if (weight > bestWeight) {
            bestWeight = weight;
            bestType = type;
        }
    }

    return contentTypeFromString(bestType);
}

Classification ContentClassifier::classify(const std::string& name, const std::vector<File>& files)
{
    Classification result;
    result.type = detectTypeFromFiles(files);

    // Adult-content sub-categorisation only applies to media-ish types.
    if (result.type == ContentType::Video || result.type == ContentType::Pictures
        || result.type == ContentType::Archive) {
        if (applyBadWords(name, result.type, result.category))
            return result; // marked Bad -- stop

        for (const File& f : files) {
            std::string path = f.path;
            const auto dotPos = path.find_last_of('.'); // strip extension, like legacy fileCheck.pop()
            if (dotPos != std::string::npos && dotPos > 0)
                path = path.substr(0, dotPos);
            if (applyBadWords(path, result.type, result.category))
                break; // Bad -- stop scanning files
        }
    }

    return result;
}

void ContentClassifier::classify(Torrent& torrent)
{
    const Classification c = classify(torrent.name, torrent.fileList);
    torrent.contentType = c.type;
    torrent.contentCategory = c.category;
}

} // namespace ratsn::domain
