# M5 implementation plan — search quality + data management

Written 2026-08-25 for the (Sonnet) session implementing M5. Read alongside
docs/DESIGN-native.md (§10 milestone row, §12 acceptance + spec-source table)
and docs/M4-PLAN.md for the conventions this plan inherits.

Scope was set by the project owner in the planning session:

- **Strict matching is ON by default**, with a visible toggle (and config key)
  to loosen back to fuzzy behavior.
- **In scope:** strict matching, TUI filter panel (+ `seedersMin`), tracker
  allow/deny filters (search-side and index-side), `ratsn export`/`import`,
  query-syntax hardening, a Top tab, a `t`-to-save-.torrent key, and index
  size control (`ratsn cleanup` + `indexMaxTorrents`, added 2026-08-25 by
  owner decision — item 8).
- **Out of scope, renumbered:** downloads + session resume is now **M6**;
  votes/feed/StorageManager is **M7**; the tracker *scrapers* (SwarmScraper
  seeders refresh + TrackerSiteScraper site metadata) are **M8**. The parked
  M4 golden-fixture TODO stays parked — do not pick it up here.

Unlike M1–M4, most of M5 is **native-only feature work with no Qt
equivalent** (strict match, export/import, tracker filters). The "port it,
don't improve it" rule still governs anything that touches wire messages or
existing ported semantics; the new features should be explicitly marked as
native extensions in comments where they extend ported code (FilterPolicy).

## Why strict matching (root cause, verified 2026-08-25)

Unrelated search results have two independent sources:

1. **Groonga match escalation.** Default `match_escalation_threshold` is 0:
   when a query gets *zero* exact hits, Groonga silently retries with unsplit
   and then partial (substring) matching. That is exactly "results without the
   full search word". None of `GroongaIndex`'s `select` commands disable it.
2. **Remote hits are appended unfiltered.** `SearchTab::onRemoteHit` dedups by
   hash and nothing else; the wire carries no query echo (M4-PLAN "Wire
   quirks"), so whatever Qt/legacy peers return lands in the list verbatim.

## Spec sources (read before writing code)

| What | Where |
|---|---|
| Groonga command construction, quoting, schema migration pattern | `native/index/groonga_index.cpp` (`quoteToken` composition comment ~line 31, `ensureSchema` objectExists-guard ~line 234) |
| Search query surface | `native/index/search_index.h` `SearchQuery` |
| TUI search flow, generation guard, remote merge | `native/tui/search_tab.cpp` |
| Tab wiring, key routing, shutdown ordering | `native/tui/app.cpp` (do not restructure the engine-thread join — see the threading note in rats-native memory/M3) |
| .torrent byte assembly (splice announce keys around the raw info dict — never re-encode it) | `src/net/torrent_engine.cpp` `assembleTorrentFile` ~line 109 and the byte-splicing comment ~lines 55–107 |
| Exporter orchestration (cache path, in-flight dedup) | `src/services/torrent_exporter.{h,cpp}` |
| Metadata fetch call + callback threading | `native/engine/crawler.cpp` `fetchMetadata` (~line 203; same `Bittorrent::get_torrent_metadata` API, callback fires on a librats worker thread) |
| Where `info.trackers` comes from | `src/services/tracker_service.cpp` `onInfoScraped`/line ~565 (`info["trackers"]` = array of names like "nyaa", "rutracker") |
| Torrent JSON schema for export/import | `native/domain/torrent_codec.cpp` (wire schema — export/import reuses it verbatim) |
| CLI subcommand shape | `native/main.cpp` `cmdAdd`/`cmdSearch` |

## Work items

### 1. Strict matching + query hardening (`GroongaIndex`, `SearchTab`, CLI)

`SearchQuery` gains `bool strict = true`.

Local (both `searchNames` and `searchFiles`):
- When strict, append `--match_escalation_threshold "-1"` to the `select`.
- Search-as-you-type stays usable via last-token prefix match: if the query
  does not end in whitespace and its last token is ≥ 3 chars and doesn't
  already end in `*`, append `*` to that token (Groonga query-syntax prefix
  search; works against the Terms `TABLE_PAT_KEY` lexicon). A trailing space
  means the user finished the word — no `*`.
- Loose mode = today's exact commands (no threshold arg), unchanged.
- Hardening, applied in BOTH modes: add `--query_flags QUERY_NO_SYNTAX_ERROR`
  to every `--query` select. This replaces the default
  `ALLOW_PRAGMA|ALLOW_COLUMN` set, so a stray `:` no longer probes columns and
  a stray `(`/`-` no longer errors the whole query. Available since Groonga
  8.0.1 — fine on both the dev VM's 13.1.1 and the target's 16.x.

Remote (strict only, client-side — the wire cannot carry it):
- `SearchTab` keeps the query text that was active when it last broadcast
  (alongside the existing `remoteSearchGeneration_`). In `onRemoteHit`,
  tokenize that text on whitespace, ASCII-casefold, strip any trailing `*`;
  accept the hit only if every token appears as a substring of the name (or of
  any `matchingPaths` entry for file hits). Substring, not word-boundary —
  cheap, and it subsumes the prefix semantics of the last token. Note in a
  comment that Groonga normalizes via NormalizerAuto (NFKC) but this check is
  plain ASCII folding — a deliberate approximation.

Surfacing:
- TUI: `strict` checkbox in the filter row (item 2), default from config.
- CLI: `ratsn search --loose` flag (strict is the default).
- Config: new native-only key `strictSearch` (default true), same
  validate/repair pattern as existing keys.

### 2. Filter panel + `seedersMin` + tracker filter (search-side)

Backend:
- `SearchQuery` gains `int seedersMin = 0` and `std::string tracker`.
- `buildFilterExpr`: `seeders >= N`; tracker filter is `trackers @ "name"`.
- New schema column: add `{ "Torrents.trackers", "column_create Torrents
  trackers COLUMN_VECTOR ShortText" }` to `ensureSchema`'s `kCore` — the
  objectExists guard makes this an automatic migration on next open of an
  existing DB. No search index on it (sequential filter is fine at current
  scale; note it as a future optimization).
- `upsert`: extract `info.trackers` (array of strings, lowercase them) into
  the new column. `rowToTorrent` needs no change (trackers stay readable via
  `info`). Existing rows have an empty column until re-upserted — replication
  churn and export→import both backfill naturally; no dedicated reindex
  command.

TUI (`search_tab.cpp`):
- A filter row under the search input, wrapped in FTXUI `Maybe(filterRow,
  &filtersVisible_)` so hidden controls can't take focus; toggled with `f`.
  Controls: type toggle (all/video/audio/pictures/books/application/archive),
  `safe` checkbox, `strict` checkbox, size min/max Inputs (accept unit
  suffixes — add a `parseSize("700M") -> int64` helper to `tui/format.h`),
  seeders-min Input, tracker Input (empty = any). Every change calls
  `triggerSearch()`.
- **Focus guard:** the `CatchEvent` at the bottom of `component()` currently
  checks only `inputFocused()` (the query Input). With more Inputs, `f`/`m`/
  `t` must not fire while ANY Input in the tab is focused — generalize to an
  `anyInputFocused()` that checks the query + the filter-row Inputs, and use
  it in `app.cpp`'s `q` guard too.
- Remote hits: size/files/seeders/type/tracker can't ride the wire (requests
  carry only safeSearch + type — `peer_api.cpp` `parseSearchRequest`), so
  apply them client-side in `onRemoteHit` via a shared
  `passesLocalFilters(hit)` helper (tracker check reads
  `hit.torrent.info["trackers"]`).

CLI: add `--seeders-min N` and `--tracker NAME` to `ratsn search`
(`--content-type` already exists).

### 3. Tracker policy (index-side, FilterPolicy extension)

New native-only config keys (empty/false defaults = today's behavior):
- `trackerAllow` (string list; non-empty = only these tracker names pass)
- `trackerDeny` (string list; matching records are rejected)
- `trackerRequireKnown` (bool; true = records with NO tracker info are
  rejected)

Applied in `native/domain/filter_policy.*` alongside the ported checks, on
`torrent.info["trackers"]` (case-insensitive compare). Mark the block with a
comment: native extension, no Qt equivalent. Rejections log through the
existing rejection-reason path (`platform::log()`), e.g.
`Indexer: rejected <hash8> (tracker policy)`.

**Honest caveat to document in the config comment and commit message:**
DHT-crawled torrents carry no tracker identity (the spider sees bare
info-hashes; BEP9 transfers only the info dict). `info.trackers` exists only
on records that passed a Qt peer with tracker-site scraping. So
`trackerAllow: ["nyaa"] + trackerRequireKnown: true` rejects *every* spider
discovery and lives off mesh replication alone — suggest `indexer: false` in
that configuration to save fetch bandwidth. M8 (site scraper port) is what
would give self-crawled torrents tracker identity.

### 4. `ratsn export` / `ratsn import`

Format: JSON Lines — one torrent per line, the exact wire-codec schema
(`codec::toJson`/`torrentFromJson`). Backend-neutral and endian-portable
(Groonga's mmap DB files are not), so it doubles as the LE↔BE migration path.

- `ratsn export [FILE|-] [--no-files]` — new `GroongaIndex::exportPage(
  int64_t afterId, int limit)` paging by `--filter "_id > N" --sort_keys _id
  --limit 1000` (cursor paging, not offset — offset is O(offset) per page).
  Output `_id` + `kOutputColumns`; `filesList` comes back via the `info`
  column automatically (upsert always stores it there — groonga_index.cpp
  ~line 324; the comment above it saying "when fileIndex is off" is stale,
  the store is unconditional — fix the comment while there). `--no-files`
  drops `fileList` from the emitted JSON (keeps the `files` count).
- `ratsn import [FILE|-] [--no-filter]` — parse each line, `torrentFromJson`,
  apply FilterPolicy (including the new tracker policy) unless `--no-filter`,
  upsert. Records are already classified (codec carries contentType/category)
  — do not re-run the classifier. Print a summary: imported / rejected(reason
  counts) / malformed.
- Both run **offline** like `add`/`search` (open the index directly, no node).
  Document in the usage line: run while `--console`/`--tui` is NOT running —
  Groonga multi-process access is supported upstream but unexercised in this
  project.
- Progress/diagnostics: data goes to stdout when FILE is `-`, so progress
  lines go to `std::cerr` here. This is a **documented exception** to the
  "always platform::log()" rule — that rule protects the TUI screen, and
  these subcommands can never run under the TUI. Say so in a comment.

### 5. Top tab

- Extract the result-list + details-pane rendering shared by Search/Top into
  `tui/result_view.{h,cpp}` (mechanical extraction of `formatResultLine`,
  `renderDetails`, selection state, and the `m`/`t` key handling from
  `search_tab.cpp` — no behavior change to the Search tab).
- New `tui/top_tab.{h,cpp}`: content-type toggle (same labels as the filter
  panel) + time toggle (all/day/week/month → `TopQuery.time` `""`/`"hours"`/
  `"week"`/`"month"` — the `"hours"` key actually means 24h in
  `groonga_index.cpp` `top()`; label it "day", keep the key). Loads via
  `index_.top()` posted to the engine thread on tab activation and on toggle
  change; no polling.
- `app.cpp`: three tabs (Search/Top/Status) — Tab-key cycling becomes modulo,
  tab bar labels updated, `/` still jumps to Search.

### 6. `t` — save .torrent for the selected result

- New `native/engine/torrent_file.{h,cpp}`: port of `assembleTorrentFile`
  (torrent_engine.cpp ~55–130): splice `announce`/`announce-list` keys around
  the **raw info-dict bytes** in bencode key order — never decode+re-encode
  the info dict (that would re-sort keys and change the infohash). Read the
  Qt file for where the announce URL list comes from and copy it.
- TUI flow: `t` on a selected result → if `<dataDir>/torrents/<hash>.torrent`
  exists, report it immediately (TorrentExporter's cache behavior) →
  otherwise post to engine thread, call `Bittorrent::get_torrent_metadata`
  (the same call `Crawler::fetchMetadata` uses — check its callback signature
  there; the callback fires on a librats worker thread, marshal via
  `EngineLoop::post` before touching anything) → assemble → write the cache
  file → `screen_.Post` a status line into the details pane ("saved …" /
  "fetch failed: …"). Dedup concurrent requests per hash with a simple
  in-flight set. Works from both Search and Top tabs (it lives in the shared
  result view).
- TUI-only: a CLI variant would need the whole node/DHT started for one
  fetch — out of scope, note why if tempted.

### 7. Config summary

New keys, all native-only, all through the existing clamp/repair path:
`strictSearch` (true), `trackerAllow` ([]), `trackerDeny` ([]),
`trackerRequireKnown` (false), `indexMaxTorrents` (0 = unlimited).

### 8. Index size control — `ratsn cleanup` + `indexMaxTorrents`

Background fact that shapes both halves: Groonga's mmap'd segment files
**grow but never shrink** — deleting records frees segments for reuse (growth
stops, inserts recycle the space) but the on-disk files keep their
high-water-mark size. So pruning means "stops growing", and actual disk
reclamation is export → import into a fresh datadir (item 4's tooling —
document this in both subcommands' usage text). This is also why the cap is
count-based, not byte-based: a byte threshold would never visibly go down
after pruning.

**`ratsn cleanup [--dry-run]`** — CLI port of the Qt `torrent.cleanup` REST
endpoint's semantics (`src/rest/api_router.cpp` ~466, read it first): sweep
the whole index with the same `_id`-keyset pagination export uses (never
OFFSET — removing rows mid-sweep shifts offsets already passed, the Qt code
comments exactly this), apply the CURRENT filter policy (including item 3's
tracker allow/deny — this is how "nyaa-only" gets enforced retroactively on
an existing index), delete rejects via `GroongaIndex::remove` (which already
cascades to the Files rows), print scanned/matched/removed. `--dry-run`
counts only. Offline subcommand like export/import (same
run-while-engine-stopped note); Qt's regex-validity pre-check (an invalid
`namingRegExp` must error out, not silently accept everything) ports too.

**`indexMaxTorrents` cap** (native extension, no Qt equivalent — mark it):
enforced by the Indexer on the engine thread so spider + replication can run
indefinitely at a bounded size. Mechanics:
- Indexer seeds a record counter from `counts()` at pipeline start and
  increments it on each genuinely-new insert (`handleDiscovered`'s existing
  bool return).
- When the counter exceeds `cap + slack` (slack = max(cap/50, 100), so
  pruning runs in occasional batches, not per-insert), post a prune task:
  new `GroongaIndex::lowestValueHashes(int limit)` — `select` sorted
  `seeders,added` ascending (zero-seeder oldest first), `output_columns
  _key` — then `remove()` each, batch-capped (~500 per task, re-post if
  still over) so a big overshoot never stalls search. Log one line per
  batch: `Indexer: pruned N (cap M)`.
- Records the mesh replicates back later simply get re-pruned next cycle —
  accepted churn, not a bug; note it in a comment.

## Acceptance (extends DESIGN-native.md §12)

- **Strict:** a query term that appears in no indexed name returns 0 results
  strict / junk loose (flip the checkbox to compare); typing a prefix of an
  indexed word still matches as-you-type; a remote hit missing a query word
  never appears while strict.
- **Filters:** size/seeders/type visibly narrow a live result list; tracker
  filter matches a replicated record carrying `info.trackers`.
- **Export/import:** export a populated index, import into a fresh datadir,
  counts match minus filter rejections; spot-check search parity; `--no-filter`
  imports everything.
- **Top tab:** seeders-sorted list; type/time toggles change it.
- **`t`:** the written .torrent opens in a real client (owner's end).
- **Cleanup:** tighten a filter (e.g. enable adult filter or a tracker allow
  list), `ratsn cleanup --dry-run` reports matches, the real run removes them
  and search no longer returns them.
- **Cap:** set `indexMaxTorrents` below the current count, run live: the
  count converges to the cap and stays there under inflow; index file sizes
  stop growing (`du` stabilizes — they will NOT shrink; that's expected).
- TUI still usable over 80×24 ssh with the filter row open.

## Session workflow constraints (unchanged from M4)

- **No local builds without asking first** — the owner builds/tests (including
  PPC). Deliver via the tarball flow: `git archive --prefix=rats-search-<short-sha>/
  --format=tar.gz -o out.tar.gz HEAD` after each commit, send with SendUserFile.
- **All engine/TUI diagnostics go through `platform::log()`** — never bare
  `std::cout`/`std::cerr` (M4 lesson; the TUI redirects that sink to
  `<dataDir>/ratsn-engine.log`). Sole exception: item 4's offline subcommands.
- When FTXUI behaves surprisingly, fetch the actual v7.0.3 source from GitHub
  (see the M3/M4 notes) instead of guessing.
- Milestones stay strictly ordered; M6 (downloads) does not start until M5's
  acceptance passes on the owner's build.
