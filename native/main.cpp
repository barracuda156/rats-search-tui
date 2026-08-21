#include "domain/torrent_codec.h"
#include "index/groonga_index.h"
#include "platform/config.h"
#include "platform/engine_loop.h"
#include "platform/paths.h"

#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using ratsn::domain::SearchHit;
using ratsn::domain::Torrent;
using ratsn::index::GroongaIndex;
using ratsn::index::GroongaRuntime;
using ratsn::index::SearchQuery;
using ratsn::index::TopQuery;
using ratsn::platform::Config;
using ratsn::platform::EngineLoop;

namespace {

std::atomic<EngineLoop*> g_engineLoop { nullptr };

void handleSigint(int)
{
    if (EngineLoop* loop = g_engineLoop.load(std::memory_order_relaxed))
        loop->stop();
}

// Pulls `--data-dir DIR` (and removes it) out of an argv-style vector so every
// subcommand's own flag parsing doesn't need to special-case it.
std::string extractDataDir(std::vector<std::string>& args)
{
    std::string dataDir;
    for (size_t i = 0; i < args.size();) {
        if (args[i] == "--data-dir" && i + 1 < args.size()) {
            dataDir = args[i + 1];
            args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i) + 2);
        } else {
            ++i;
        }
    }
    return dataDir;
}

bool nextFlagValue(const std::vector<std::string>& args, const std::string& flag, std::string& out)
{
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == flag && i + 1 < args.size()) {
            out = args[i + 1];
            return true;
        }
    }
    return false;
}

bool hasFlag(const std::vector<std::string>& args, const std::string& flag)
{
    for (const std::string& a : args) {
        if (a == flag)
            return true;
    }
    return false;
}

std::unique_ptr<GroongaIndex> openIndex(const std::filesystem::path& dataDir, std::string* error)
{
    const Config cfg = Config::load(ratsn::platform::configFile(dataDir));
    return GroongaIndex::open(ratsn::platform::indexDir(dataDir), cfg.fileIndex, error);
}

void printTorrentLine(const Torrent& t)
{
    std::cout << t.hash << "  seeders=" << t.seeders << "  size=" << t.size << "  " << t.name << "\n";
}

int cmdConsole(std::vector<std::string> args)
{
    const std::string dataDirArg = extractDataDir(args);
    const std::filesystem::path dataDir = ratsn::platform::resolveDataDir(dataDirArg);
    const std::filesystem::path cfgPath = ratsn::platform::configFile(dataDir);

    Config cfg = Config::load(cfgPath);
    if (!std::filesystem::exists(cfgPath))
        cfg.save(cfgPath);

    GroongaRuntime runtime;
    std::string error;
    std::unique_ptr<GroongaIndex> index = GroongaIndex::open(ratsn::platform::indexDir(dataDir), cfg.fileIndex, &error);
    if (!index) {
        std::cerr << "ratsn: failed to open index: " << error << "\n";
        return 1;
    }

    EngineLoop loop;
    g_engineLoop.store(&loop, std::memory_order_relaxed);
    std::signal(SIGINT, handleSigint);
    std::signal(SIGTERM, handleSigint);

    const auto stats = index->counts();
    std::cout << "ratsn --console: data dir " << dataDir.string() << ", fileIndex=" << (cfg.fileIndex ? "on" : "off")
               << "\nindexed " << stats.torrents << " torrents, " << stats.files << " files. Ctrl-C to exit.\n";

    loop.run();
    g_engineLoop.store(nullptr, std::memory_order_relaxed);
    std::cout << "\nratsn: shutting down\n";
    return 0;
}

int cmdSearch(std::vector<std::string> args)
{
    const std::string dataDirArg = extractDataDir(args);
    if (args.empty()) {
        std::cerr << "usage: ratsn search <query> [--files] [--limit N] [--offset N] [--sort KEY] [--asc]\n"
                     "                     [--content-type T] [--size-min N] [--size-max N]\n"
                     "                     [--files-min N] [--files-max N] [--safe-search] [--json]\n";
        return 2;
    }

    SearchQuery q;
    q.text = args.front();
    args.erase(args.begin());

    std::string v;
    if (nextFlagValue(args, "--limit", v))
        q.limit = std::stoi(v);
    if (nextFlagValue(args, "--offset", v))
        q.offset = std::stoi(v);
    if (nextFlagValue(args, "--sort", v))
        q.sort = v;
    if (hasFlag(args, "--asc"))
        q.descending = false;
    if (nextFlagValue(args, "--content-type", v))
        q.contentType = v;
    if (nextFlagValue(args, "--size-min", v))
        q.sizeMin = std::stoll(v);
    if (nextFlagValue(args, "--size-max", v))
        q.sizeMax = std::stoll(v);
    if (nextFlagValue(args, "--files-min", v))
        q.filesMin = std::stoi(v);
    if (nextFlagValue(args, "--files-max", v))
        q.filesMax = std::stoi(v);
    if (hasFlag(args, "--safe-search"))
        q.safeSearch = true;
    const bool searchFiles = hasFlag(args, "--files");
    const bool asJson = hasFlag(args, "--json");

    const std::filesystem::path dataDir = ratsn::platform::resolveDataDir(dataDirArg);
    GroongaRuntime runtime;
    std::string error;
    std::unique_ptr<GroongaIndex> index = openIndex(dataDir, &error);
    if (!index) {
        std::cerr << "ratsn: failed to open index: " << error << "\n";
        return 1;
    }

    const std::vector<SearchHit> hits = searchFiles ? index->searchFiles(q) : index->searchNames(q);
    for (const SearchHit& hit : hits) {
        if (asJson)
            std::cout << ratsn::domain::codec::toJson(hit, { true, true }).dump() << "\n";
        else
            printTorrentLine(hit.torrent);
    }
    if (!asJson)
        std::cout << hits.size() << " result(s)\n";
    return 0;
}

int cmdAdd(std::vector<std::string> args)
{
    const std::string dataDirArg = extractDataDir(args);
    if (args.empty()) {
        std::cerr << "usage: ratsn add <torrent.json> [torrent2.json ...]\n";
        return 2;
    }

    const std::filesystem::path dataDir = ratsn::platform::resolveDataDir(dataDirArg);
    GroongaRuntime runtime;
    std::string error;
    std::unique_ptr<GroongaIndex> index = openIndex(dataDir, &error);
    if (!index) {
        std::cerr << "ratsn: failed to open index: " << error << "\n";
        return 1;
    }

    int failures = 0;
    for (const std::string& path : args) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::cerr << "ratsn: cannot read " << path << "\n";
            ++failures;
            continue;
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        librats::Json j = librats::Json::parse(buf.str(), nullptr, false);
        if (j.is_discarded()) {
            std::cerr << "ratsn: malformed JSON in " << path << "\n";
            ++failures;
            continue;
        }
        Torrent t = ratsn::domain::codec::torrentFromJson(j);
        if (!index->upsert(t)) {
            std::cerr << "ratsn: failed to index " << path << " (hash=" << t.hash << ")\n";
            ++failures;
            continue;
        }
        std::cout << "indexed " << t.hash << "  " << t.name << "\n";
    }
    return failures == 0 ? 0 : 1;
}

int cmdTop(std::vector<std::string> args)
{
    const std::string dataDirArg = extractDataDir(args);
    TopQuery q;
    std::string v;
    if (nextFlagValue(args, "--content-type", v))
        q.contentType = v;
    if (nextFlagValue(args, "--time", v))
        q.time = v;
    if (nextFlagValue(args, "--limit", v))
        q.limit = std::stoi(v);
    if (nextFlagValue(args, "--offset", v))
        q.offset = std::stoi(v);

    const std::filesystem::path dataDir = ratsn::platform::resolveDataDir(dataDirArg);
    GroongaRuntime runtime;
    std::string error;
    std::unique_ptr<GroongaIndex> index = openIndex(dataDir, &error);
    if (!index) {
        std::cerr << "ratsn: failed to open index: " << error << "\n";
        return 1;
    }
    for (const Torrent& t : index->top(q))
        printTorrentLine(t);
    return 0;
}

int cmdRandom(std::vector<std::string> args)
{
    const std::string dataDirArg = extractDataDir(args);
    int limit = 5;
    std::string v;
    if (nextFlagValue(args, "--limit", v))
        limit = std::stoi(v);

    const std::filesystem::path dataDir = ratsn::platform::resolveDataDir(dataDirArg);
    GroongaRuntime runtime;
    std::string error;
    std::unique_ptr<GroongaIndex> index = openIndex(dataDir, &error);
    if (!index) {
        std::cerr << "ratsn: failed to open index: " << error << "\n";
        return 1;
    }
    for (const Torrent& t : index->random(limit))
        printTorrentLine(t);
    return 0;
}

void printUsage()
{
    std::cerr << "usage: ratsn <command> [--data-dir DIR] [options]\n"
                 "commands:\n"
                 "  --console            run the (currently idle) engine loop until Ctrl-C\n"
                 "  search <query>       query the local index\n"
                 "  add <file.json...>   hand-load one or more torrent JSON records\n"
                 "  top                  list top torrents by seeders\n"
                 "  random               sample random torrents\n";
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        printUsage();
        return 2;
    }

    const std::string command = args.front();
    args.erase(args.begin());

    try {
        if (command == "--console")
            return cmdConsole(std::move(args));
        if (command == "search")
            return cmdSearch(std::move(args));
        if (command == "add")
            return cmdAdd(std::move(args));
        if (command == "top")
            return cmdTop(std::move(args));
        if (command == "random")
            return cmdRandom(std::move(args));
        if (command == "-h" || command == "--help") {
            printUsage();
            return 0;
        }
    } catch (const std::exception& e) {
        std::cerr << "ratsn: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "ratsn: unknown command '" << command << "'\n";
    printUsage();
    return 2;
}
