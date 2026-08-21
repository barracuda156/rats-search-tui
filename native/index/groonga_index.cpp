#include "index/groonga_index.h"

#include "domain/torrent_codec.h"

#include <groonga.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <random>

namespace ratsn::index {

namespace {

const std::string kOutputColumns = "_key,name,size,files,piece_length,added,content_type,content_category,"
                                    "seeders,leechers,completed,good,bad,trackers_checked,info";

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool isHashLike(const std::string& s)
{
    return s.size() == 40 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c); });
}

// Escapes and double-quotes `raw` into a single token for Groonga's
// command-line request grammar (verified against lib/str.c's
// grn_text_unesc_tok(): '\\' and '"' need backslash-escaping inside a
// double-quoted token; everything else passes through as UTF-8 bytes).
// Applying this once produces an outer command token; applying it to a
// string that is itself going to be used as a quoted string literal inside a
// --filter/--query expression (Groonga's own, separately-parsed grammar) and
// then applying it AGAIN to the whole expression composes the two layers
// correctly — see the call sites below.
std::string quoteToken(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size() + 2);
    out.push_back('"');
    for (char c : raw) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    out.push_back('"');
    return out;
}

std::string arg(const std::string& flag, const std::string& value)
{
    return " --" + flag + " " + quoteToken(value);
}

std::string join(const std::vector<std::string>& parts, const std::string& sep)
{
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i)
            out += sep;
        out += parts[i];
    }
    return out;
}

std::string contentTypeFilterExpr(const std::string& type)
{
    if (type.empty())
        return {};
    if (type == "application") {
        return "in_values(content_type, " + std::to_string(domain::toId(domain::ContentType::Software)) + ", "
            + std::to_string(domain::toId(domain::ContentType::Games)) + ")";
    }
    return "content_type == " + std::to_string(domain::toId(domain::contentTypeFromString(type)));
}

// Body of a create-style command (table_create/column_create) is a plain
// boolean; a select/load/delete response is validated by send()'s rc check
// alone (which already turns any command failure into a null response here),
// so this is only used right after ensureSchema()'s DDL sends.
bool commandOk(const librats::Json& response)
{
    if (response.is_null())
        return false;
    return !response.is_boolean() || response.get<bool>();
}

} // namespace

// ---------------------------------------------------------------------------
// GroongaRuntime
// ---------------------------------------------------------------------------

GroongaRuntime::GroongaRuntime()
{
    grn_init();
}

GroongaRuntime::~GroongaRuntime()
{
    grn_fin();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

GroongaIndex::GroongaIndex(_grn_ctx* ctx, _grn_obj* db, bool file_index) : ctx_(ctx), db_(db), fileIndex_(file_index) { }

GroongaIndex::~GroongaIndex()
{
    if (ctx_) {
        if (db_)
            grn_obj_close(ctx_, db_);
        grn_ctx_close(ctx_);
    }
}

std::unique_ptr<GroongaIndex> GroongaIndex::open(const std::filesystem::path& db_path, bool file_index, std::string* error)
{
    std::error_code ec;
    std::filesystem::create_directories(db_path, ec);
    if (ec) {
        if (error)
            *error = "cannot create " + db_path.string() + ": " + ec.message();
        return nullptr;
    }

    grn_ctx* ctx = grn_ctx_open(0);
    if (!ctx) {
        if (error)
            *error = "grn_ctx_open failed";
        return nullptr;
    }

    const std::filesystem::path dbFile = db_path / "db";
    grn_obj* db = grn_db_open(ctx, dbFile.string().c_str());
    if (!db) {
        ctx->rc = GRN_SUCCESS; // grn_db_open's failure (likely "no such db") shouldn't leak into grn_db_create
        db = grn_db_create(ctx, dbFile.string().c_str(), nullptr);
    }
    if (!db) {
        if (error)
            *error = std::string("cannot open/create groonga db: ") + ctx->errbuf;
        grn_ctx_close(ctx);
        return nullptr;
    }

    auto index = std::unique_ptr<GroongaIndex>(new GroongaIndex(ctx, db, file_index));
    if (!index->ensureSchema(error))
        return nullptr;
    return index;
}

// ---------------------------------------------------------------------------
// Command transport
// ---------------------------------------------------------------------------

librats::Json GroongaIndex::send(const std::string& command, std::string* error)
{
    grn_ctx* ctx = ctx_;
    grn_ctx_send(ctx, command.data(), static_cast<unsigned int>(command.size()), 0);
    // grn_ctx_recv() below resets ctx->rc/errbuf as it drains the response, so the
    // send-time outcome — the only place a command failure is actually reported in
    // the embedded API (there is no [[rc,started,elapsed],body] envelope; that's
    // CLI/server-only output formatting, never produced by grn_ctx_send/recv) —
    // has to be captured now.
    const grn_rc sendRc = ctx->rc;
    const std::string sendErr = ctx->errbuf;

    std::string responseText;
    for (;;) {
        char* buf = nullptr;
        unsigned int bufLen = 0;
        int flags = 0;
        if (grn_ctx_recv(ctx, &buf, &bufLen, &flags) != GRN_SUCCESS) {
            if (error)
                *error = ctx->errbuf[0] ? ctx->errbuf : "grn_ctx_recv failed";
            return librats::Json();
        }
        if (bufLen > 0)
            responseText.append(buf, bufLen);
        if (!(flags & GRN_CTX_MORE))
            break;
    }

    if (sendRc != GRN_SUCCESS) {
        if (error)
            *error = sendErr;
        return librats::Json();
    }

    librats::Json parsed = librats::Json::parse(responseText, nullptr, false);
    if (parsed.is_discarded()) {
        if (error)
            *error = "malformed groonga response: " + responseText;
        return librats::Json();
    }
    return parsed;
}

bool GroongaIndex::objectExists(const std::string& name)
{
    grn_obj* obj = grn_ctx_get(ctx_, name.c_str(), static_cast<int>(name.size()));
    return obj != nullptr;
}

// ---------------------------------------------------------------------------
// Schema (docs/DESIGN-native.md §5 — strings copied verbatim from the
// validated smoke-test schema)
// ---------------------------------------------------------------------------

bool GroongaIndex::ensureSchema(std::string* error)
{
    struct Obj {
        const char* name;
        const char* ddl;
    };
    static const Obj kCore[] = {
        { "Torrents", "table_create Torrents TABLE_HASH_KEY ShortText" },
        { "Torrents.name", "column_create Torrents name COLUMN_SCALAR ShortText" },
        { "Torrents.size", "column_create Torrents size COLUMN_SCALAR UInt64" },
        { "Torrents.files", "column_create Torrents files COLUMN_SCALAR UInt32" },
        { "Torrents.piece_length", "column_create Torrents piece_length COLUMN_SCALAR UInt32" },
        { "Torrents.added", "column_create Torrents added COLUMN_SCALAR Time" },
        { "Torrents.content_type", "column_create Torrents content_type COLUMN_SCALAR UInt8" },
        { "Torrents.content_category", "column_create Torrents content_category COLUMN_SCALAR UInt8" },
        { "Torrents.seeders", "column_create Torrents seeders COLUMN_SCALAR UInt32" },
        { "Torrents.leechers", "column_create Torrents leechers COLUMN_SCALAR UInt32" },
        { "Torrents.completed", "column_create Torrents completed COLUMN_SCALAR UInt32" },
        { "Torrents.good", "column_create Torrents good COLUMN_SCALAR UInt32" },
        { "Torrents.bad", "column_create Torrents bad COLUMN_SCALAR UInt32" },
        { "Torrents.trackers_checked", "column_create Torrents trackers_checked COLUMN_SCALAR Time" },
        { "Torrents.info", "column_create Torrents info COLUMN_SCALAR Text" },
        { "Terms", "table_create Terms TABLE_PAT_KEY ShortText --default_tokenizer TokenBigram "
                   "--normalizer NormalizerAuto" },
        { "Terms.idx_name", "column_create Terms idx_name COLUMN_INDEX|WITH_POSITION Torrents name" },
    };
    for (const Obj& o : kCore) {
        if (objectExists(o.name))
            continue;
        std::string err;
        if (!commandOk(send(o.ddl, &err))) {
            if (error)
                *error = std::string("schema setup failed on `") + o.ddl + "`: " + err;
            return false;
        }
    }

    if (fileIndex_) {
        static const Obj kFiles[] = {
            { "Files", "table_create Files TABLE_NO_KEY" },
            { "Files.torrent", "column_create Files torrent COLUMN_SCALAR Torrents" },
            { "Files.path", "column_create Files path COLUMN_SCALAR ShortText" },
            { "Files.size", "column_create Files size COLUMN_SCALAR UInt64" },
            { "Terms.idx_path", "column_create Terms idx_path COLUMN_INDEX|WITH_POSITION Files path" },
        };
        for (const Obj& o : kFiles) {
            if (objectExists(o.name))
                continue;
            std::string err;
            if (!commandOk(send(o.ddl, &err))) {
                if (error)
                    *error = std::string("schema setup failed on `") + o.ddl + "`: " + err;
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Write path
// ---------------------------------------------------------------------------

bool GroongaIndex::upsert(const domain::Torrent& t)
{
    if (!t.isValid())
        return false;

    librats::Json rec = librats::Json::object();
    rec["_key"] = toLower(t.hash);
    rec["name"] = t.name;
    rec["size"] = t.size;
    rec["files"] = t.files;
    rec["piece_length"] = t.pieceLength;
    rec["added"] = static_cast<double>(t.added) / 1000.0;
    rec["content_type"] = domain::toId(t.contentType);
    rec["content_category"] = domain::toId(t.contentCategory);
    rec["seeders"] = t.seeders;
    rec["leechers"] = t.leechers;
    rec["completed"] = t.completed;
    rec["good"] = t.good;
    rec["bad"] = t.bad;
    if (t.trackersChecked > 0)
        rec["trackers_checked"] = static_cast<double>(t.trackersChecked) / 1000.0;

    // `info` is a Text column: a JSON-encoded string, not a nested Groonga
    // object (§5's schema comment). It carries the scraped extras plus, when
    // fileIndex is off, the file list itself (still stored for display, just
    // not FT-indexed).
    librats::Json infoOut = t.info.is_object() ? t.info : librats::Json::object();
    if (!t.fileList.empty())
        infoOut["filesList"] = domain::codec::filesToJson(t.fileList);
    if (!infoOut.empty())
        rec["info"] = infoOut.dump();

    librats::Json values = librats::Json::array();
    values.push_back(std::move(rec));

    const std::string cmd = "load --table Torrents --values " + quoteToken(values.dump());
    std::string err;
    if (send(cmd, &err).is_null())
        return false;

    if (fileIndex_) {
        removeFilesFor(t.hash);
        if (!t.fileList.empty()) {
            librats::Json fileValues = librats::Json::array();
            for (const domain::File& f : t.fileList) {
                librats::Json row = librats::Json::object();
                row["torrent"] = toLower(t.hash);
                row["path"] = f.path;
                row["size"] = f.size;
                fileValues.push_back(std::move(row));
            }
            send("load --table Files --values " + quoteToken(fileValues.dump()));
        }
    }
    return true;
}

void GroongaIndex::removeFilesFor(const std::string& hash)
{
    const std::string filterExpr = "torrent == " + quoteToken(toLower(hash));
    send("delete --table Files --filter " + quoteToken(filterExpr));
}

bool GroongaIndex::remove(const std::string& hash)
{
    if (fileIndex_)
        removeFilesFor(hash);
    const std::string cmd = "delete --table Torrents --key " + quoteToken(toLower(hash));
    std::string err;
    return !send(cmd, &err).is_null();
}

bool GroongaIndex::updateStats(const std::string& hash, int seeders, int leechers, int completed)
{
    librats::Json rec = librats::Json::object();
    rec["_key"] = toLower(hash);
    rec["seeders"] = seeders;
    rec["leechers"] = leechers;
    rec["completed"] = completed;
    librats::Json values = librats::Json::array();
    values.push_back(std::move(rec));

    const std::string cmd = "load --table Torrents --values " + quoteToken(values.dump());
    std::string err;
    return !send(cmd, &err).is_null();
}

// ---------------------------------------------------------------------------
// Read path
// ---------------------------------------------------------------------------

std::string GroongaIndex::buildFilterExpr(const SearchQuery& q) const
{
    std::vector<std::string> parts;
    if (q.safeSearch)
        parts.push_back("content_category != " + std::to_string(domain::toId(domain::ContentCategory::XXX)));
    if (const std::string ct = contentTypeFilterExpr(q.contentType); !ct.empty())
        parts.push_back(ct);
    if (q.sizeMin > 0)
        parts.push_back("size > " + std::to_string(q.sizeMin));
    if (q.sizeMax > 0)
        parts.push_back("size < " + std::to_string(q.sizeMax));
    if (q.filesMin > 0)
        parts.push_back("files > " + std::to_string(q.filesMin));
    if (q.filesMax > 0)
        parts.push_back("files < " + std::to_string(q.filesMax));
    return join(parts, " && ");
}

std::string GroongaIndex::resolveSortColumn(const std::string& key)
{
    static const std::vector<std::string> kAllowed = { "seeders", "leechers", "name", "size", "files", "added",
        "completed" };
    const std::string lower = toLower(key);
    for (const std::string& a : kAllowed) {
        if (a == lower)
            return a;
    }
    return {};
}

const librats::Json* GroongaIndex::firstResultSet(const librats::Json& response)
{
    // A `select` response body is itself `[[[nhits],[[col,type],...],row,...], ...]`
    // (one element per drilldown) — no further envelope wraps it (see send()).
    if (!response.is_array() || response.empty())
        return nullptr;
    const librats::Json& resultSet = response[0];
    if (!resultSet.is_array() || resultSet.size() < 2 || !resultSet[0].is_array() || !resultSet[1].is_array())
        return nullptr;
    return &resultSet;
}

domain::Torrent GroongaIndex::rowToTorrent(const std::vector<std::string>& columns, const librats::Json& row)
{
    domain::Torrent t;
    if (!row.is_array())
        return t;

    for (size_t i = 0; i < columns.size() && i < row.size(); ++i) {
        const std::string& col = columns[i];
        const librats::Json& v = row[i];
        if (col == "_key")
            t.hash = v.get<std::string>();
        else if (col == "name")
            t.name = v.get<std::string>();
        else if (col == "size")
            t.size = v.get<int64_t>();
        else if (col == "files")
            t.files = v.get<int>();
        else if (col == "piece_length")
            t.pieceLength = v.get<int>();
        else if (col == "added")
            t.added = static_cast<int64_t>(std::llround(v.get<double>() * 1000.0));
        else if (col == "content_type")
            t.contentType = domain::contentTypeFromId(v.get<int>());
        else if (col == "content_category")
            t.contentCategory = domain::contentCategoryFromId(v.get<int>());
        else if (col == "seeders")
            t.seeders = v.get<int>();
        else if (col == "leechers")
            t.leechers = v.get<int>();
        else if (col == "completed")
            t.completed = v.get<int>();
        else if (col == "good")
            t.good = v.get<int>();
        else if (col == "bad")
            t.bad = v.get<int>();
        else if (col == "trackers_checked") {
            const double secs = v.get<double>();
            t.trackersChecked = secs > 0 ? static_cast<int64_t>(std::llround(secs * 1000.0)) : 0;
        } else if (col == "info") {
            const std::string infoText = v.get<std::string>();
            if (!infoText.empty()) {
                librats::Json infoJson = librats::Json::parse(infoText, nullptr, false);
                if (infoJson.is_object()) {
                    if (const librats::Json* fl = infoJson.as_object().find("filesList"))
                        t.fileList = domain::codec::filesFromJson(*fl);
                    infoJson.erase("filesList");
                    t.info = std::move(infoJson);
                }
            }
        }
    }
    if (t.files == 0 && !t.fileList.empty())
        t.files = static_cast<int>(t.fileList.size());
    return t;
}

std::vector<domain::Torrent> GroongaIndex::parseSelectRows(const librats::Json& response)
{
    std::vector<domain::Torrent> out;
    const librats::Json* resultSet = firstResultSet(response);
    if (!resultSet)
        return out;

    std::vector<std::string> columns;
    for (const librats::Json& c : (*resultSet)[1]) {
        if (c.is_array() && !c.empty())
            columns.push_back(c[0].get<std::string>());
    }

    for (size_t i = 2; i < resultSet->size(); ++i)
        out.push_back(rowToTorrent(columns, (*resultSet)[i]));
    return out;
}

int64_t GroongaIndex::extractFirstId(const librats::Json& response)
{
    const librats::Json* resultSet = firstResultSet(response);
    if (!resultSet || resultSet->size() < 3)
        return 0;
    const librats::Json& row = (*resultSet)[2];
    if (!row.is_array() || row.empty())
        return 0;
    return row[0].get<int64_t>();
}

std::vector<domain::SearchHit> GroongaIndex::searchNames(const SearchQuery& q)
{
    std::vector<domain::SearchHit> hits;
    if (q.text.empty())
        return hits;

    const bool isHash = isHashLike(q.text);

    std::vector<std::string> filterParts;
    if (isHash)
        filterParts.push_back("_key == " + quoteToken(toLower(q.text)));
    if (const std::string common = buildFilterExpr(q); !common.empty())
        filterParts.push_back(common);
    const std::string filterExpr = join(filterParts, " && ");

    std::string cmd = "select --table Torrents";
    cmd += arg("output_columns", kOutputColumns);
    if (!filterExpr.empty())
        cmd += arg("filter", filterExpr);
    if (!isHash) {
        cmd += arg("match_columns", "name");
        cmd += arg("query", q.text);
    }

    const std::string sortCol = resolveSortColumn(q.sort);
    std::string sortKeys;
    if (!sortCol.empty())
        sortKeys = (q.descending ? "-" : "") + sortCol;
    else if (!isHash)
        sortKeys = "-_score";
    if (!sortKeys.empty())
        cmd += arg("sort_keys", sortKeys);

    cmd += arg("offset", std::to_string(q.offset));
    cmd += arg("limit", std::to_string(q.limit));

    for (domain::Torrent& t : parseSelectRows(send(cmd))) {
        domain::SearchHit hit;
        hit.torrent = std::move(t);
        hits.push_back(std::move(hit));
    }
    return hits;
}

std::vector<domain::SearchHit> GroongaIndex::searchFiles(const SearchQuery& q)
{
    std::vector<domain::SearchHit> hits;
    if (q.text.empty() || !fileIndex_)
        return hits;

    std::string cmd = "select --table Files";
    cmd += arg("output_columns", "torrent._key,path");
    cmd += arg("match_columns", "path");
    cmd += arg("query", q.text);
    cmd += arg("offset", std::to_string(q.offset));
    cmd += arg("limit", std::to_string(q.limit));

    const librats::Json response = send(cmd);
    const librats::Json* resultSet = firstResultSet(response);
    if (!resultSet)
        return hits;

    std::vector<std::pair<std::string, std::vector<std::string>>> orderedMatches; // hash -> paths, insertion order
    for (size_t i = 2; i < resultSet->size(); ++i) {
        const librats::Json& row = (*resultSet)[i];
        if (!row.is_array() || row.size() < 2)
            continue;
        const std::string hash = row[0].get<std::string>();
        const std::string path = row[1].get<std::string>();
        auto it = std::find_if(orderedMatches.begin(), orderedMatches.end(),
            [&](const auto& e) { return e.first == hash; });
        if (it == orderedMatches.end())
            orderedMatches.push_back({ hash, { path } });
        else
            it->second.push_back(path);
    }
    if (orderedMatches.empty())
        return hits;

    std::vector<std::string> hashFilterParts;
    for (const auto& entry : orderedMatches)
        hashFilterParts.push_back(quoteToken(entry.first));
    std::string parentFilter = "in_values(_key, " + join(hashFilterParts, ", ") + ")";
    if (const std::string ct = contentTypeFilterExpr(q.contentType); !ct.empty())
        parentFilter += " && " + ct;

    std::string parentCmd = "select --table Torrents";
    parentCmd += arg("output_columns", kOutputColumns);
    parentCmd += arg("filter", parentFilter);
    parentCmd += arg("limit", std::to_string(orderedMatches.size()));

    for (domain::Torrent& t : parseSelectRows(send(parentCmd))) {
        if (q.safeSearch && t.contentCategory == domain::ContentCategory::XXX)
            continue;
        auto it = std::find_if(orderedMatches.begin(), orderedMatches.end(),
            [&](const auto& e) { return e.first == t.hash; });
        domain::SearchHit hit;
        hit.fromFileMatch = true;
        if (it != orderedMatches.end()) {
            hit.matchingPaths = it->second;
            for (const std::string& p : it->second)
                t.fileList.push_back(domain::File { p, 0 });
        }
        hit.torrent = std::move(t);
        hits.push_back(std::move(hit));
    }

    if (const std::string col = resolveSortColumn(q.sort); !col.empty()) {
        std::stable_sort(hits.begin(), hits.end(), [&](const domain::SearchHit& a, const domain::SearchHit& b) {
            if (col == "seeders")
                return q.descending ? a.torrent.seeders > b.torrent.seeders : a.torrent.seeders < b.torrent.seeders;
            if (col == "size")
                return q.descending ? a.torrent.size > b.torrent.size : a.torrent.size < b.torrent.size;
            return false;
        });
    }
    return hits;
}

std::vector<domain::Torrent> GroongaIndex::top(const TopQuery& q)
{
    std::vector<std::string> parts;
    parts.push_back("seeders > 0");
    parts.push_back("content_category != " + std::to_string(domain::toId(domain::ContentCategory::XXX)));
    if (const std::string ct = contentTypeFilterExpr(q.contentType); !ct.empty())
        parts.push_back(ct);

    if (!q.time.empty()) {
        const int64_t nowSec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
                                    .count();
        int64_t cutoff = 0;
        if (q.time == "hours")
            cutoff = nowSec - 60 * 60 * 24;
        else if (q.time == "week")
            cutoff = nowSec - 60 * 60 * 24 * 7;
        else if (q.time == "month")
            cutoff = nowSec - 60LL * 60 * 24 * 30;
        if (cutoff > 0)
            parts.push_back("added > " + std::to_string(cutoff));
    }

    std::string cmd = "select --table Torrents";
    cmd += arg("output_columns", kOutputColumns);
    cmd += arg("filter", join(parts, " && "));
    cmd += arg("sort_keys", "-seeders");
    cmd += arg("offset", std::to_string(q.offset));
    cmd += arg("limit", std::to_string(q.limit));

    return parseSelectRows(send(cmd));
}

std::vector<domain::Torrent> GroongaIndex::random(int limit)
{
    std::vector<domain::Torrent> out;
    if (limit <= 0)
        return out;

    std::string maxCmd = "select --table Torrents";
    maxCmd += arg("output_columns", "_id");
    maxCmd += arg("sort_keys", "-_id");
    maxCmd += arg("limit", "1");
    const int64_t maxId = extractFirstId(send(maxCmd));
    if (maxId <= 0)
        return out;

    std::mt19937_64 rng(std::random_device {}());
    std::uniform_int_distribution<int64_t> dist(1, maxId);
    const int64_t oversample = std::min<int64_t>(maxId, static_cast<int64_t>(limit) * 5 + 10);

    std::vector<std::string> ids;
    ids.reserve(oversample);
    for (int64_t i = 0; i < oversample; ++i)
        ids.push_back(std::to_string(dist(rng)));

    const std::string filterExpr = "in_values(_id, " + join(ids, ", ") + ") && seeders > 0 && content_category != "
        + std::to_string(domain::toId(domain::ContentCategory::XXX));

    std::string cmd = "select --table Torrents";
    cmd += arg("output_columns", kOutputColumns);
    cmd += arg("filter", filterExpr);
    cmd += arg("limit", std::to_string(limit));

    return parseSelectRows(send(cmd));
}

IndexStats GroongaIndex::counts()
{
    IndexStats stats;
    std::string cmd = "select --table Torrents";
    cmd += arg("output_columns", "size,files");
    cmd += arg("limit", "-1");

    const librats::Json response = send(cmd);
    const librats::Json* resultSet = firstResultSet(response);
    if (!resultSet)
        return stats;

    if (!(*resultSet)[0].empty())
        stats.torrents = (*resultSet)[0][0].get<int64_t>();

    for (size_t i = 2; i < resultSet->size(); ++i) {
        const librats::Json& row = (*resultSet)[i];
        if (!row.is_array() || row.size() < 2)
            continue;
        stats.totalSize += row[0].get<int64_t>();
        stats.files += row[1].get<int64_t>();
    }
    return stats;
}

} // namespace ratsn::index
