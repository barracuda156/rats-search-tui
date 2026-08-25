// Golden-file wire-compat check (docs/M4-PLAN.md). Every JSON fixture in
// native/tests/fixtures/ is a message captured from a live Qt rats-search
// instance (RATSN_WIRE_DUMP=1, see engine/peer_api.cpp's dumpWire, and
// fixtures/README.md for the capture procedure). Each fixture is
// round-tripped through the native codec (torrentFromJson -> toJson) and
// compared against the original as parsed JSON trees, field-by-field and
// order-insensitive -- a field the native codec drops or renames is a
// failure (docs/DESIGN-native.md §11's schema-drift guard).
//
// No gtest: nothing new to port to the retro (PowerPC) target. Built behind
// RATSN_BUILD_TESTS (native/CMakeLists.txt, default OFF); run directly or
// via ctest.

#include "domain/torrent_codec.h"
#include "librats/util/json.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

// Compares two JSON trees, order-insensitive for objects, reporting every
// mismatch (not just the first) so a fixture failure is fully diagnosable
// from one run's output.
bool jsonEquals(const librats::Json& a, const librats::Json& b, const std::string& path)
{
    if (a.is_object() && b.is_object()) {
        bool ok = true;
        for (const auto& [key, value] : a.as_object()) {
            const librats::Json* other = b.as_object().find(key);
            if (!other) {
                std::cerr << "  " << path << "." << key << ": missing on the round-tripped side\n";
                ok = false;
                continue;
            }
            if (!jsonEquals(value, *other, path + "." + key))
                ok = false;
        }
        for (const auto& [key, value] : b.as_object()) {
            (void)value;
            if (!a.as_object().find(key)) {
                std::cerr << "  " << path << "." << key << ": introduced by the round trip (not in the fixture)\n";
                ok = false;
            }
        }
        return ok;
    }
    if (a.is_array() && b.is_array()) {
        if (a.size() != b.size()) {
            std::cerr << "  " << path << ": array size " << a.size() << " != " << b.size() << "\n";
            return false;
        }
        bool ok = true;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (!jsonEquals(a[i], b[i], path + "[" + std::to_string(i) + "]"))
                ok = false;
        }
        return ok;
    }
    // String comparison rather than Json::operator== so a numeric subtype
    // difference (e.g. Integer vs Unsigned, both printing "5") that arose
    // purely from decode/re-encode isn't flagged as a schema-drift failure.
    if (a.dump() != b.dump()) {
        std::cerr << "  " << path << ": " << a.dump() << " != " << b.dump() << "\n";
        return false;
    }
    return true;
}

librats::Json readJsonFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return librats::Json::parse(buf.str(), nullptr, false);
}

void checkFixture(const std::filesystem::path& path)
{
    const std::string name = path.filename().string();
    std::cout << "checking " << name << "...\n";

    const librats::Json original = readJsonFile(path);
    if (original.is_discarded() || !original.is_object()) {
        std::cerr << "FAIL: " << name << ": not a valid JSON object\n";
        ++failures;
        return;
    }

    const ratsn::domain::Torrent torrent = ratsn::domain::codec::torrentFromJson(original);
    if (torrent.hash.empty()) {
        std::cerr << "FAIL: " << name << ": torrentFromJson produced no hash\n";
        ++failures;
        return;
    }

    // A fixture with an embedded file list round-trips it back out; one
    // without shouldn't gain one from the encoder's own defaults.
    const bool hadFiles = original.contains("files_list") || original.contains("filesList");
    const librats::Json roundTripped
        = ratsn::domain::codec::toJson(torrent, { /*includeFiles*/ hadFiles, /*includeInfo*/ true });

    if (!jsonEquals(original, roundTripped, name)) {
        std::cerr << "FAIL: " << name << ": round trip mismatch\n";
        ++failures;
    }
}

} // namespace

int main()
{
    const std::filesystem::path fixturesDir = std::filesystem::path(RATSN_TEST_FIXTURES_DIR);
    if (!std::filesystem::exists(fixturesDir)) {
        std::cerr << "wire_codec_test: fixtures dir " << fixturesDir.string() << " missing\n";
        return 1;
    }

    std::vector<std::filesystem::path> fixtures;
    for (const auto& entry : std::filesystem::directory_iterator(fixturesDir)) {
        if (entry.path().extension() == ".json")
            fixtures.push_back(entry.path());
    }

    if (fixtures.empty()) {
        std::cout << "wire_codec_test: no fixtures yet in " << fixturesDir.string() << ".\n"
                   << "Capture some with RATSN_WIRE_DUMP=1 against a live rats-search peer and\n"
                   << "curate them here -- see fixtures/README.md. Nothing to check; passing.\n";
        return 0;
    }

    for (const auto& fixture : fixtures)
        checkFixture(fixture);

    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }

    std::cout << fixtures.size() << " fixture(s) OK\n";
    return 0;
}
