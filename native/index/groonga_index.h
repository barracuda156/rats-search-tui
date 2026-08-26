#pragma once

#include "index/search_index.h"
#include "librats/util/json.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// grn_ctx / grn_obj, from <groonga/groonga.h> — kept out of this header.
struct _grn_ctx;
struct _grn_obj;

namespace ratsn::index {

// grn_init()/grn_fin() are process-global and must bracket every GroongaIndex
// (docs/DESIGN-native.md §12). Construct one in main(), before opening any
// index, and keep it alive until the last GroongaIndex is destroyed.
class GroongaRuntime {
public:
    GroongaRuntime();
    ~GroongaRuntime();

    GroongaRuntime(const GroongaRuntime&) = delete;
    GroongaRuntime& operator=(const GroongaRuntime&) = delete;
};

// Embedded libgroonga index over the schema in docs/DESIGN-native.md §5.
// Owns one grn_ctx; not thread-safe (grn_ctx isn't) — confine all calls to
// the EngineLoop thread, per §3.
class GroongaIndex : public SearchIndex {
public:
    // `db_path` is the directory to hold the Groonga database (e.g.
    // <datadir>/index); it is created if missing. `file_index` gates whether
    // the Files table + path index are created (§5's 32-bit budget note).
    static std::unique_ptr<GroongaIndex> open(
        const std::filesystem::path& db_path, bool file_index, std::string* error = nullptr);

    ~GroongaIndex() override;

    GroongaIndex(const GroongaIndex&) = delete;
    GroongaIndex& operator=(const GroongaIndex&) = delete;

    bool upsert(const domain::Torrent& torrent) override;
    bool remove(const std::string& hash) override;

    std::vector<domain::SearchHit> searchNames(const SearchQuery& query) override;
    std::vector<domain::SearchHit> searchFiles(const SearchQuery& query) override;
    std::vector<domain::Torrent> top(const TopQuery& query) override;
    std::vector<domain::Torrent> random(int limit) override;

    bool updateStats(const std::string& hash, int seeders, int leechers, int completed) override;
    bool mergeInfo(const std::string& hash, const librats::Json& info) override;
    bool updateVotes(const std::string& hash, int good, int bad) override;
    IndexStats counts() override;
    std::vector<std::string> lowestValueHashes(int limit) override;

    bool fileIndexEnabled() const { return fileIndex_; }

    struct IdTorrent {
        int64_t id = 0;
        domain::Torrent torrent;
    };
    // Cursor-paginated full sweep by internal `_id`, ascending, `_id >
    // afterId`, up to `limit` rows -- never OFFSET (docs/M5-PLAN.md item 8:
    // removing rows mid-sweep would shift the offsets already passed).
    // Shared primitive behind both `ratsn export` and `ratsn cleanup`; not
    // part of the SearchIndex interface (an offline, Groonga-cursor-specific
    // admin op, always called through the concrete type in main.cpp).
    std::vector<IdTorrent> pageAfterId(int64_t afterId, int limit);

private:
    GroongaIndex(_grn_ctx* ctx, _grn_obj* db, bool file_index);

    bool ensureSchema(std::string* error);

    // Sends one command string and returns its parsed response body (the
    // embedded C API has no CLI-style `[[rc,started,elapsed],body]` envelope
    // — grn_ctx_send/recv return the command's raw output). On a send-time
    // rc error or transport error, returns a null Json and sets *error; does
    // not itself validate the body's shape.
    librats::Json send(const std::string& command, std::string* error = nullptr);
    // Looks up a table or "Table.column" by name in the DB's flat object
    // namespace; used to make schema setup idempotent across restarts.
    bool objectExists(const std::string& name);
    void removeFilesFor(const std::string& hash);
    // Raw (still JSON-encoded-string) `info` column for one hash, or an empty
    // string if the row doesn't exist or has none -- the read half of
    // mergeInfo's read-merge-write.
    std::string readInfoRaw(const std::string& hash);

    std::string buildFilterExpr(const SearchQuery& query) const;
    static std::string resolveSortColumn(const std::string& key);
    static domain::Torrent rowToTorrent(const std::vector<std::string>& columns, const librats::Json& row);
    static std::vector<domain::Torrent> parseSelectRows(const librats::Json& response);
    static int64_t extractFirstId(const librats::Json& response);
    // The `[[nhits], [[col,type],...], row, row, ...]` element of a select
    // response's body, or null if `response` isn't a well-formed select reply.
    static const librats::Json* firstResultSet(const librats::Json& response);

    _grn_ctx* ctx_ = nullptr;
    _grn_obj* db_ = nullptr;
    bool fileIndex_ = false;
};

} // namespace ratsn::index
