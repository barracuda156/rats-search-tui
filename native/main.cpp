#include "domain/torrent_codec.h"
#include "engine/crawler.h"
#include "engine/downloads.h"
#include "engine/indexer.h"
#include "engine/node_host.h"
#include "engine/peer_api.h"
#include "engine/peer_registry.h"
#include "engine/replication.h"
#include "index/groonga_index.h"
#include "platform/config.h"
#include "platform/engine_loop.h"
#include "platform/paths.h"
#ifdef RATSN_WITH_TUI
#include "tui/app.h"
#endif

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef _WIN32
#include <sys/resource.h>
#endif

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

// Periodic "torrents=N files=N discovered=N dhtNodes=N" line -- the M2
// deliverable's explicit stats-line requirement (docs/DESIGN-native.md §10).
// Self-reschedules like Crawler's own timers; stops naturally once the loop
// is asked to stop.
class StatsPrinter {
public:
    StatsPrinter(EngineLoop& loop, GroongaIndex& index, ratsn::engine::Crawler* crawler, ratsn::engine::NodeHost* nodeHost,
        ratsn::engine::DownloadManager* downloads)
        : loop_(loop)
        , index_(index)
        , crawler_(crawler)
        , nodeHost_(nodeHost)
        , downloads_(downloads)
    {
    }

    void start(int intervalMs)
    {
        intervalMs_ = intervalMs;
        schedule();
    }

private:
    void schedule() { loop_.postDelayed([this] { tick(); }, intervalMs_); }

    void tick()
    {
        if (!loop_.isRunning())
            return;
        const auto stats = index_.counts();
        std::cout << "stats: torrents=" << stats.torrents << " files=" << stats.files;
        if (crawler_)
            std::cout << " discovered=" << crawler_->discoveredCount() << " activeFetches=" << crawler_->activeFetches();
        if (nodeHost_) {
            std::cout << " dhtNodes=" << nodeHost_->dhtNodeCount() << " spiderPool=" << nodeHost_->spiderPoolSize()
                       << " spiderVisited=" << nodeHost_->spiderVisitedCount() << " peers=" << nodeHost_->peerCount();
        }
        if (downloads_) {
            const auto agg = downloads_->aggregate();
            std::cout << " dl=" << agg.active << " dlSpeed=" << static_cast<int64_t>(agg.downloadSpeed) << "B/s";
        }
        std::cout << "\n";
        schedule();
    }

    EngineLoop& loop_;
    GroongaIndex& index_;
    ratsn::engine::Crawler* crawler_;
    ratsn::engine::NodeHost* nodeHost_;
    ratsn::engine::DownloadManager* downloads_;
    int intervalMs_ = 5000;
};

void handleSigint(int)
{
    if (EngineLoop* loop = g_engineLoop.load(std::memory_order_relaxed))
        loop->stop();
}

// Raises the process's open-file soft limit toward its hard ceiling, best
// effort. macOS ships a stingy per-process default (commonly 256) that a
// long-running node can exhaust in ordinary operation: DHT + mesh +
// BitTorrent peer sockets, every active download's open piece files,
// Groonga's own mmap'd segment files, plus ratsn's own log files all count
// against it. Past the limit, ANY open()/socket() call anywhere in the
// process can fail with EMFILE -- this crashed a real M6 test session via
// std::random_device's constructor throwing uncaught out of
// GroongaIndex::random() (that call site is separately hardened not to
// crash on it, but the fd exhaustion itself needed fixing at the source).
// Mirrors the pattern librats/tests/test_reactor.cpp already uses for the
// identical reason (see its raise_fd_limit()), minus the test's specific
// target size. Only called from --console/--tui: the one-shot CLI
// subcommands don't hold long-lived connections and don't need it.
void raiseFdLimit()
{
#ifndef _WIN32
    rlimit rl {};
    if (::getrlimit(RLIMIT_NOFILE, &rl) != 0)
        return;
    constexpr rlim_t kWanted = 8192;
    if (rl.rlim_cur >= kWanted)
        return;
    rlimit want = rl;
    want.rlim_cur = (rl.rlim_max == RLIM_INFINITY) ? kWanted : std::min(rl.rlim_max, kWanted);
    ::setrlimit(RLIMIT_NOFILE, &want); // best-effort; failure just leaves the original limit
#endif
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

// Shared by startEnginePipeline (live filtering), cmdImport and cmdCleanup
// (docs/M5-PLAN.md items 3/4/8) so the same config keys drive filtering
// everywhere, instead of three independently-maintained copies of this
// block.
ratsn::domain::FilterSettings filterSettingsFromConfig(const Config& cfg)
{
    ratsn::domain::FilterSettings fs;
    fs.maxFiles = cfg.filters.maxFiles;
    fs.sizeMin = cfg.filters.sizeMin;
    fs.sizeMax = cfg.filters.sizeMax;
    fs.adultFilter = cfg.filters.adultFilter;
    fs.namingRegExp = cfg.filters.namingRegExp;
    fs.namingRegExpNegative = cfg.filters.namingRegExpNegative;
    fs.contentTypeFilter = cfg.filters.contentType;
    fs.trackerAllow = cfg.trackerAllow;
    fs.trackerDeny = cfg.trackerDeny;
    fs.trackerRequireKnown = cfg.trackerRequireKnown;
    return fs;
}

// Config::downloadPath (Qt key, docs/M6-PLAN.md item 4): empty falls back to
// $HOME/Downloads when set, else <dataDir>/downloads. Qt uses the OS
// download location via QStandardPaths; this is the closest portable
// equivalent for a target (retro PowerPC) with no such standard directory.
std::string resolveDownloadPath(const Config& cfg, const std::filesystem::path& dataDir)
{
    if (!cfg.downloadPath.empty())
        return cfg.downloadPath;
    if (const char* home = std::getenv("HOME"); home && *home)
        return (std::filesystem::path(home) / "Downloads").string();
    return (dataDir / "downloads").string();
}

// Client version advertised to peers in the client_info handshake
// (PeerRegistry); RATSN_VERSION is a CMake compile definition (native/CMakeLists.txt).
constexpr const char* kClientVersion = "ratsn/" RATSN_VERSION;

// NodeHost (DHT + BitTorrent + P2P mesh) -> Crawler (discovery + BEP9
// metadata) -> Indexer (classify -> filter -> upsert), plus the M4 mesh
// pieces (PeerApi, Replication) layered on NodeHost's MessageJson, and (M6)
// DownloadManager on NodeHost's BitTorrent client. Shared by --console and
// tui so the two commands' setup can't drift apart.
//
// Unlike the pre-M6 SpiderPipeline this replaces, NodeHost/mesh/downloads
// start regardless of cfg.spider -- downloads need the BitTorrent client
// independent of whether DHT-crawl discovery is enabled (docs/M6-PLAN.md
// item 1, mirroring Qt's application.cpp: transport always starts, only the
// crawler is gated). Only `crawler` stays null when cfg.spider is off. On a
// node start failure, prints the same diagnostic --console has always
// printed and leaves nodeHost/crawler/replication/peerApi/downloads unable
// to do anything (downloads reports "not ready", same as the mesh already
// did) -- the pipeline degrades to index-only rather than failing the whole
// command.
struct EnginePipeline {
    std::unique_ptr<ratsn::engine::NodeHost> nodeHost;
    std::unique_ptr<ratsn::engine::Indexer> indexer;
    std::unique_ptr<ratsn::engine::Crawler> crawler;
    std::unique_ptr<ratsn::engine::Replication> replication;
    std::unique_ptr<ratsn::engine::PeerApi> peerApi;
    std::unique_ptr<ratsn::engine::DownloadManager> downloads;
};

EnginePipeline startEnginePipeline(const Config& cfg, const std::filesystem::path& dataDir, GroongaIndex& index, EngineLoop& loop)
{
    EnginePipeline p;

    p.indexer = std::make_unique<ratsn::engine::Indexer>(
        index, filterSettingsFromConfig(cfg), loop, cfg.indexMaxTorrents);

    p.nodeHost = std::make_unique<ratsn::engine::NodeHost>(cfg, dataDir, loop, kClientVersion);
    if (!p.nodeHost->start()) {
        std::cerr << "ratsn: failed to start node/DHT; spider/mesh/downloads disabled\n";
        p.nodeHost.reset();
        p.downloads = std::make_unique<ratsn::engine::DownloadManager>(nullptr, loop, resolveDownloadPath(cfg, dataDir));
        return p;
    }

    if (cfg.spider) {
        p.crawler = std::make_unique<ratsn::engine::Crawler>(p.nodeHost->bittorrent(), loop);
        p.crawler->setWalkInterval(cfg.walkInterval);
        ratsn::engine::Indexer* indexerPtr = p.indexer.get();
        p.crawler->setKnownHashFilter([indexerPtr](const std::string& hash) { return indexerPtr->isKnownHash(hash); });
        p.crawler->setDiscoveredCallback([indexerPtr](const Torrent& t) { indexerPtr->handleDiscovered(t); });
        if (!p.crawler->start()) {
            std::cerr << "ratsn: failed to start crawler\n";
            p.crawler.reset();
        }
    }

    // Mesh: peer_api handlers + replication ask-loop (M4). messageJson() is
    // only null if node_->start() somehow attached it but the node is not
    // actually running, which start()'s own failure path above already
    // ruled out.
    if (auto* messages = p.nodeHost->messageJson()) {
        ratsn::engine::NodeHost* nodeHostPtr = p.nodeHost.get();
        p.replication = std::make_unique<ratsn::engine::Replication>(
            *messages, loop, [nodeHostPtr] { return nodeHostPtr->peerCount(); });
        p.replication->setEnabled(cfg.p2pReplication);
        if (cfg.p2pReplication)
            p.replication->start();

        p.peerApi = std::make_unique<ratsn::engine::PeerApi>(
            *messages, index, *p.indexer, loop, p.replication.get(), cfg.p2pReplicationServer);
        if (std::getenv("RATSN_WIRE_DUMP"))
            p.peerApi->enableWireDump(dataDir);

        if (ratsn::engine::PeerRegistry* registry = p.nodeHost->peerRegistry()) {
            ratsn::engine::PeerApi* peerApiPtr = p.peerApi.get();
            registry->setPeerConnectedCallback(
                [peerApiPtr](const std::string& peerId) { peerApiPtr->onPeerConnected(peerId); });
        }
    }

    // Downloads (M6): needs only the BitTorrent client, independent of
    // spider/mesh -- guards its own operations on nodeHost->bittorrent()
    // being available.
    p.downloads = std::make_unique<ratsn::engine::DownloadManager>(
        p.nodeHost.get(), loop, resolveDownloadPath(cfg, dataDir));

    return p;
}

// Debug-only localhost pairing (docs/M4-PLAN.md): parses "host:port" and
// dials it directly, bypassing DHT/PEX discovery. No-op (with a diagnostic)
// if the mesh isn't up or the target is malformed.
void connectDebugPeer(const EnginePipeline& pipeline, const std::string& target)
{
    if (!pipeline.nodeHost) {
        std::cerr << "ratsn: --connect requires the node to be running (see the startup error above)\n";
        return;
    }
    const size_t colon = target.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= target.size()) {
        std::cerr << "ratsn: invalid --connect target '" << target << "' (expected host:port)\n";
        return;
    }
    const std::string host = target.substr(0, colon);
    int port = 0;
    try {
        port = std::stoi(target.substr(colon + 1));
    } catch (const std::exception&) {
        std::cerr << "ratsn: invalid --connect port in '" << target << "'\n";
        return;
    }
    std::cout << "ratsn: connecting to " << host << ":" << port << "\n";
    pipeline.nodeHost->connectTo(host, static_cast<uint16_t>(port));
}

void printTorrentLine(const Torrent& t)
{
    std::cout << t.hash << "  seeders=" << t.seeders << "  size=" << t.size << "  " << t.name << "\n";
}

int cmdConsole(std::vector<std::string> args)
{
    raiseFdLimit();
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

    EnginePipeline pipeline = startEnginePipeline(cfg, dataDir, *index, loop);

    std::string connectTarget;
    if (nextFlagValue(args, "--connect", connectTarget))
        connectDebugPeer(pipeline, connectTarget);

    // Restore any in-progress downloads (mirrors application.cpp:252, run
    // after the pipeline/transport is up). loadSession/start no-op cleanly
    // when the BitTorrent client isn't available (nodeHost start failed).
    const std::filesystem::path sessionFile = ratsn::platform::downloadsFile(dataDir);
    pipeline.downloads->loadSession(sessionFile.string());
    pipeline.downloads->start(sessionFile.string());

    const auto stats = index->counts();
    std::cout << "ratsn --console: data dir " << dataDir.string() << ", fileIndex=" << (cfg.fileIndex ? "on" : "off")
               << ", spider=" << (pipeline.crawler ? "on" : "off")
               << "\nindexed " << stats.torrents << " torrents, " << stats.files << " files. Ctrl-C to exit.\n";

    StatsPrinter statsPrinter(loop, *index, pipeline.crawler.get(), pipeline.nodeHost.get(), pipeline.downloads.get());
    statsPrinter.start(5000);

    loop.run();

    if (pipeline.crawler)
        pipeline.crawler->stop();
    // Downloads save before the node (its BitTorrent client) stops --
    // mirrors application.cpp's shutdown ordering (docs/M6-PLAN.md item 4).
    pipeline.downloads->stop();
    pipeline.downloads->saveSession(sessionFile.string());
    if (pipeline.nodeHost)
        pipeline.nodeHost->stop();

    g_engineLoop.store(nullptr, std::memory_order_relaxed);
    std::cout << "\nratsn: shutting down\n";
    return 0;
}

#ifdef RATSN_WITH_TUI
int cmdTui(std::vector<std::string> args)
{
    raiseFdLimit();
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

    // Unlike --console (which runs the EngineLoop on the calling thread),
    // the TUI needs the calling thread free for FTXUI's own blocking event
    // loop -- so the EngineLoop runs on a background thread instead
    // (docs/DESIGN-native.md §3). SIGINT is also handled by FTXUI itself by
    // default (Ctrl-C); registering handleSigint here too is a redundant
    // safety net, same as --console, and covers SIGTERM which FTXUI doesn't.
    EngineLoop loop;
    g_engineLoop.store(&loop, std::memory_order_relaxed);
    std::signal(SIGINT, handleSigint);
    std::signal(SIGTERM, handleSigint);

    EnginePipeline pipeline = startEnginePipeline(cfg, dataDir, *index, loop);

    std::string connectTarget;
    if (nextFlagValue(args, "--connect", connectTarget))
        connectDebugPeer(pipeline, connectTarget);

    // Restore any in-progress downloads (mirrors application.cpp:252, run
    // after the pipeline/transport is up). loadSession/start no-op cleanly
    // when the BitTorrent client isn't available (nodeHost start failed).
    const std::filesystem::path sessionFile = ratsn::platform::downloadsFile(dataDir);
    pipeline.downloads->loadSession(sessionFile.string());
    pipeline.downloads->start(sessionFile.string());

    ratsn::tui::StatusInfo info;
    info.dataDir = dataDir.string();
    info.spiderEnabled = static_cast<bool>(pipeline.crawler);
    info.fileIndexEnabled = cfg.fileIndex;
    if (pipeline.nodeHost) {
        info.nodeId = pipeline.nodeHost->nodeIdShort();
        info.listenPort = pipeline.nodeHost->listenPort();
    }

    std::thread engineThread([&loop] { loop.run(); });
    // Stops and joins engineThread itself before returning -- see tui/app.cpp.
    ratsn::tui::run(loop, engineThread, *index, pipeline.nodeHost.get(), pipeline.crawler.get(), pipeline.peerApi.get(),
        pipeline.downloads.get(), cfg, info);

    if (pipeline.crawler)
        pipeline.crawler->stop();
    // Downloads save before the node (its BitTorrent client) stops --
    // mirrors application.cpp's shutdown ordering (docs/M6-PLAN.md item 4).
    pipeline.downloads->stop();
    pipeline.downloads->saveSession(sessionFile.string());
    if (pipeline.nodeHost)
        pipeline.nodeHost->stop();

    g_engineLoop.store(nullptr, std::memory_order_relaxed);
    return 0;
}
#endif

int cmdSearch(std::vector<std::string> args)
{
    const std::string dataDirArg = extractDataDir(args);
    if (args.empty()) {
        std::cerr << "usage: ratsn search <query> [--files] [--limit N] [--offset N] [--sort KEY] [--asc]\n"
                     "                     [--content-type T] [--size-min N] [--size-max N]\n"
                     "                     [--files-min N] [--files-max N] [--seeders-min N] [--tracker NAME]\n"
                     "                     [--safe-search] [--loose] [--json]\n";
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
    if (nextFlagValue(args, "--seeders-min", v))
        q.seedersMin = std::stoi(v);
    if (nextFlagValue(args, "--tracker", v))
        q.tracker = v;
    if (hasFlag(args, "--safe-search"))
        q.safeSearch = true;
    if (hasFlag(args, "--loose"))
        q.strict = false;
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

// Groups a FilterPolicy rejection message (e.g. "Size too small: 12 < 100")
// down to its stable prefix (up to the first ':', or the whole string for a
// message with no dynamic suffix) so cmdImport/cmdCleanup can print
// meaningful counts per *rule* instead of one bucket per distinct message.
std::string reasonCategory(const std::string& reason)
{
    const size_t colon = reason.find(':');
    return colon == std::string::npos ? reason : reason.substr(0, colon);
}

// `ratsn export`/`ratsn import`/`ratsn cleanup` (docs/M5-PLAN.md item 4/8)
// run offline like `add`/`search` (open the index directly, no node) --
// run them while --console/--tui is NOT running (Groonga multi-process
// access is supported upstream but unexercised in this project). All three
// stream JSON to/from stdin/stdout, so progress/diagnostics go to std::cerr
// here -- a deliberate, documented exception to the "always platform::log()"
// rule (that rule protects the TUI screen; these subcommands can never run
// under the TUI).
int cmdExport(std::vector<std::string> args)
{
    const std::string dataDirArg = extractDataDir(args);
    const bool noFiles = hasFlag(args, "--no-files");
    args.erase(std::remove(args.begin(), args.end(), "--no-files"), args.end());
    const std::string outPath = args.empty() ? "-" : args.front();

    const std::filesystem::path dataDir = ratsn::platform::resolveDataDir(dataDirArg);
    GroongaRuntime runtime;
    std::string error;
    std::unique_ptr<GroongaIndex> index = openIndex(dataDir, &error);
    if (!index) {
        std::cerr << "ratsn: failed to open index: " << error << "\n";
        return 1;
    }

    std::ofstream file;
    if (outPath != "-") {
        file.open(outPath, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::cerr << "ratsn: cannot write " << outPath << "\n";
            return 1;
        }
    }
    std::ostream* out = outPath == "-" ? &std::cout : &file;

    ratsn::domain::codec::ToJsonOptions options;
    options.includeFiles = !noFiles;
    options.includeInfo = true;

    constexpr int kBatch = 1000;
    int64_t afterId = 0;
    int64_t total = 0;
    for (;;) {
        const auto batch = index->pageAfterId(afterId, kBatch);
        if (batch.empty())
            break;
        afterId = batch.back().id;
        for (const auto& item : batch) {
            *out << ratsn::domain::codec::toJson(item.torrent, options).dump() << "\n";
            ++total;
        }
        std::cerr << "ratsn export: " << total << " written\r";
    }
    std::cerr << "\nratsn export: done, " << total << " torrent(s)\n";
    return 0;
}

int cmdImport(std::vector<std::string> args)
{
    const std::string dataDirArg = extractDataDir(args);
    const bool noFilter = hasFlag(args, "--no-filter");
    args.erase(std::remove(args.begin(), args.end(), "--no-filter"), args.end());
    const std::string inPath = args.empty() ? "-" : args.front();

    const std::filesystem::path dataDir = ratsn::platform::resolveDataDir(dataDirArg);
    const Config cfg = Config::load(ratsn::platform::configFile(dataDir));
    GroongaRuntime runtime;
    std::string error;
    std::unique_ptr<GroongaIndex> index = GroongaIndex::open(ratsn::platform::indexDir(dataDir), cfg.fileIndex, &error);
    if (!index) {
        std::cerr << "ratsn: failed to open index: " << error << "\n";
        return 1;
    }

    // FilterPolicy applied here includes the tracker allow/deny policy (item
    // 3) -- torrents already classified by the exporting side (codec carries
    // contentType/category) are not re-run through the classifier.
    const ratsn::domain::FilterPolicy policy(filterSettingsFromConfig(cfg));

    std::ifstream file;
    if (inPath != "-") {
        file.open(inPath, std::ios::binary);
        if (!file) {
            std::cerr << "ratsn: cannot read " << inPath << "\n";
            return 1;
        }
    }
    std::istream* in = inPath == "-" ? &std::cin : &file;

    int imported = 0;
    int malformed = 0;
    std::unordered_map<std::string, int> rejectReasons;
    std::string line;
    int64_t lineNo = 0;
    while (std::getline(*in, line)) {
        ++lineNo;
        if (line.empty())
            continue;
        librats::Json j = librats::Json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            ++malformed;
            continue;
        }
        Torrent t = ratsn::domain::codec::torrentFromJson(j);
        if (t.hash.length() != 40) {
            ++malformed;
            continue;
        }
        if (!noFilter) {
            if (const std::string reason = policy.rejectionReason(t); !reason.empty()) {
                ++rejectReasons[reasonCategory(reason)];
                continue;
            }
        }
        if (index->upsert(t))
            ++imported;
        else
            ++malformed;
        if (lineNo % 1000 == 0)
            std::cerr << "ratsn import: " << lineNo << " lines processed\r";
    }

    int rejected = 0;
    for (const auto& [reason, count] : rejectReasons)
        rejected += count;
    std::cerr << "\nratsn import: imported=" << imported << " rejected=" << rejected << " malformed=" << malformed << "\n";
    for (const auto& [reason, count] : rejectReasons)
        std::cerr << "  rejected (" << count << "): " << reason << "\n";
    return 0;
}

// CLI port of the Qt `torrent.cleanup` REST endpoint (src/rest/api_router.cpp
// ~466): re-applies the CURRENT filter policy across the whole index and
// removes torrents that no longer pass -- e.g. after tightening the adult/
// size filters, or retroactively enforcing a new tracker allow list (item 3)
// on records that were indexed before it was configured.
int cmdCleanup(std::vector<std::string> args)
{
    const std::string dataDirArg = extractDataDir(args);
    const bool dryRun = hasFlag(args, "--dry-run");

    const std::filesystem::path dataDir = ratsn::platform::resolveDataDir(dataDirArg);
    const Config cfg = Config::load(ratsn::platform::configFile(dataDir));

    const ratsn::domain::FilterSettings fs = filterSettingsFromConfig(cfg);
    // An invalid regex must error out, not silently accept everything (the
    // Qt handler's exact rationale -- see FilterPolicy::isValidNamingRegExp).
    std::string regexError;
    if (!ratsn::domain::FilterPolicy::isValidNamingRegExp(fs.namingRegExp, &regexError)) {
        std::cerr << "ratsn: invalid namingRegExp: " << regexError << "\n";
        return 1;
    }
    const ratsn::domain::FilterPolicy policy(fs);

    GroongaRuntime runtime;
    std::string error;
    std::unique_ptr<GroongaIndex> index = openIndex(dataDir, &error);
    if (!index) {
        std::cerr << "ratsn: failed to open index: " << error << "\n";
        return 1;
    }

    // Keyset pagination by `_id`, never OFFSET: removing rows mid-sweep would
    // shift the offsets already passed (same reasoning as the Qt handler).
    constexpr int kBatch = 500;
    int64_t afterId = 0;
    int64_t scanned = 0;
    int64_t matched = 0;
    for (;;) {
        const auto batch = index->pageAfterId(afterId, kBatch);
        if (batch.empty())
            break;
        afterId = batch.back().id;
        for (const auto& item : batch) {
            ++scanned;
            if (!policy.accepts(item.torrent)) {
                ++matched;
                if (!dryRun)
                    index->remove(item.torrent.hash);
            }
        }
        std::cerr << "ratsn cleanup: scanned=" << scanned << " matched=" << matched << "\r";
    }
    std::cerr << "\nratsn cleanup: " << (dryRun ? "dry-run, " : "") << "scanned=" << scanned << " matched=" << matched
               << " removed=" << (dryRun ? 0 : matched) << "\n";
    return 0;
}

void printUsage()
{
    std::cerr << "usage: ratsn <command> [--data-dir DIR] [options]\n"
                 "commands:\n"
                 "  --console [--connect HOST:PORT]   run the engine loop until Ctrl-C\n"
#ifdef RATSN_WITH_TUI
                 "  --tui [--connect HOST:PORT]       interactive terminal UI (search/downloads/status) on the live index\n"
#endif
                 "  search <query>       query the local index\n"
                 "  add <file.json...>   hand-load one or more torrent JSON records\n"
                 "  top                  list top torrents by seeders\n"
                 "  random               sample random torrents\n"
                 "  export [FILE|-] [--no-files]     dump the index as JSON Lines (default: stdout)\n"
                 "  import [FILE|-] [--no-filter]    load JSON Lines back in (default: stdin)\n"
                 "  cleanup [--dry-run]              re-apply the filter policy, remove what no longer passes\n"
                 "\n"
                 "export/import/cleanup run OFFLINE (open the index directly, no node) -- run them\n"
                 "while --console/--tui is NOT running.\n";
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
#ifdef RATSN_WITH_TUI
        if (command == "--tui")
            return cmdTui(std::move(args));
#endif
        if (command == "search")
            return cmdSearch(std::move(args));
        if (command == "add")
            return cmdAdd(std::move(args));
        if (command == "top")
            return cmdTop(std::move(args));
        if (command == "random")
            return cmdRandom(std::move(args));
        if (command == "export")
            return cmdExport(std::move(args));
        if (command == "import")
            return cmdImport(std::move(args));
        if (command == "cleanup")
            return cmdCleanup(std::move(args));
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
