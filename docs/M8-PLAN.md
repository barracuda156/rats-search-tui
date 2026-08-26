# M8 implementation plan — tracker scrapers (swarm stats + site metadata)

Written 2026-08-26. Independent of M7 (different files; the only overlap is
trivial main.cpp/config wiring) — M7 and M8 can run in either order, each in
its own session, whenever the owner says so. Read alongside
docs/DESIGN-native.md (§10 milestone row, §12 acceptance).

M8 ports the two tracker scrapers, fixing the known stats gap: native
`GroongaIndex::updateStats` exists but is **never called** — seeders/
leechers are frozen at discovery/replication time, so Top-tab ordering and
the details pane go stale (this is also what made the M5 `t`-key
investigation confusing: "50 seeders" entries with zero live peers).

The governing rule applies — **the Qt code is the behavioral spec: port it,
don't improve it** — with the usual architectural translations (EngineLoop
confinement instead of mutexes, worker threads instead of QThreadPool,
libcurl instead of QNetworkAccessManager, PCRE2 instead of
QRegularExpression).

## Dependency + optionality policy (owner's call, 2026-08-26)

- **libcurl is a hard build dependency** of ratsn as of M8. Ordinary
  standard libraries are freely available on every target including
  PPC/10.6 (owner's own ports overlay); do NOT wrap it in a CMake option.
  (cpr is an acceptable convenience wrapper if the implementing session
  prefers it; this plan assumes plain libcurl easy handles — the scraper
  needs exactly GET + redirects + gzip + timeout + cancellation.)
- **Runtime config keys, not compile options, gate heavy non-essential
  functionality.** Site scraping is the heavy half (a TLS handshake + page
  fetch + regex parse per indexed torrent at crawl rate, and merged
  descriptions grow the index on disk) — it gets its own runtime key
  (deviation #2). Swarm-count announces are cheap UDP and stay under Qt's
  existing master key only.

## The two halves

| | Swarm stats (`SwarmScraper`) | Site metadata (`TrackerSiteScraper`) |
|---|---|---|
| What | seeders/leechers via tracker announces (numwant=0 announce doubles as scrape) | description/category/thread-ids/tracker identity scraped from rutracker.org + nyaa.si HTML |
| Transport | librats `announce_to_tracker` (BEP 3 HTTP + BEP 15 UDP, blocking, cancellable) — Qt already uses it, zero new deps | HTTPS GET (libcurl) + regex parsing (PCRE2) |
| Writes | `updateStats` + `trackers_checked` stamp | `mergeInfo` into the `info` column |
| Why it matters | live Top-tab/details counts | gives DHT-crawled torrents a tracker identity — closes M5's documented gap where tracker allow/deny filters only work via replication |

## Spec sources (read before writing code)

| What | Where |
|---|---|
| Swarm scraper: default tracker list (librats bundles none — copy it), announceOne (numwant=0, port 6881, event None, left 0), per-hash 5-min cooldown + prune, concurrency cap 5 + FIFO queue, ScrapeState best-result fold, stop semantics (cancel-poll → drain) | `src/net/swarm_scraper.{h,cpp}` |
| Site scraper: rutracker + nyaa strategies (URLs, regex extraction, nyaa search→view two-step), merged info JSON keys (`poster`, `description`, `contentCategory`, `trackers[]`, `rutrackerThreadId`, `nyaaThreadId`, `trackerName`), 1-h cooldown, cap 3, why the cap exists (decompressed pages are the dominant heap consumer) | `src/net/tracker_site_scraper.{h,cpp}` |
| Glue service: enabled flags, `onTorrentIndexed` → checkCounts+checkInfo, scraped → repository writes, stop ordering (flags first, then drain scrapers) | `src/services/tracker_service.cpp` |
| Persistence semantics: counts update also stamps `trackersChecked=now` (native updateStats does NOT yet — extend it); info is a shallow key-merge into the existing `info` object | `src/data/torrent_repository.cpp` `updateTrackerCounts` (~425), `mergeInfo` (~454) |
| Triggers: every successfully indexed torrent (`torrentIndexed` signal), plus a view-triggered refresh when a torrent's details are shown (counts always; site scrape only when `info.trackers` is empty), plus the REST `tracker.check` shape (not ported — no REST) | `src/app/application.cpp` ~164/~200, `src/ui/torrentdetailspanel.cpp` `requestTrackerRefresh` (~798), `src/rest/api_router.cpp` ~740 |
| Config gate | `src/app/config_store.cpp` `trackersEnabled` (key `trackers`, default true) |
| librats announce API: `TrackerRequest`/`TrackerResponse`, `announce_to_tracker(url, req, timeout, CancelPoll)`, `generate_peer_id` | `src/librats/src/librats/bittorrent/tracker.h`, `types.h` |
| Native write targets | `native/index/groonga_index.cpp` `updateStats` (~435, extend), `info` column handling in upsert (~324) |

## Work items

### 1. `native/engine/swarm_scraper.{h,cpp}` (~250 loc)

Port of SwarmScraper with the state simplified onto the EngineLoop:

- **Bookkeeping (cooldown map, pending FIFO, active count) is
  EngineLoop-confined — no mutexes.** `requestScrape` is only ever called
  from the engine thread (indexer hook, TUI posts). Qt's three mutexes
  exist because Qt callers arrive on arbitrary threads; the native
  translation is the established no-mutex pattern.
- **Announce execution on a small dedicated worker pool** (fixed
  `std::thread`s + condition-variable task queue, max 16 threads matching
  Qt's `kMaxPoolThreads`; tasks are single-tracker blocking
  `announce_to_tracker` calls). Port `ScrapeState` as-is
  (`shared_ptr` + mutex + atomic remaining; the fold keeps the best
  successful result across the hash's trackers). The finishing task posts
  the aggregate back to the EngineLoop.
- Constants and semantics verbatim: `kDefaultTrackers` list (copy it — the
  librats core bundles none), 15 s per announce, 5-min per-hash cooldown
  with opportunistic pruning, cap 5 concurrent hashes. Queue drain: on
  each completion post, start queued hashes up to the cap (replaces Qt's
  500 ms poll timer — same effect, translation).
- `stop()`: set `stopping_` (checked by the CancelPoll and by the
  per-tracker loop, so in-flight work bails after at most the announce in
  progress), clear the queue, notify + join the pool. Call it in the
  shutdown sequence before `nodeHost->stop()`.
- Result callback (engine thread): `scraped(hash, seeders, leechers,
  completed)` — completed is always 0 from an announce (Qt comment: not
  reported), keep that.

### 2. `native/engine/site_scraper.{h,cpp}` (~450 loc)

Port of TrackerSiteScraper on the same worker-pool pattern:

- HTTP via **libcurl easy handles** on the pool threads:
  `CURLOPT_FOLLOWLOCATION`, `CURLOPT_ACCEPT_ENCODING ""` (Qt inflates
  gzip/zstd transparently — curl's empty string enables all built-in
  decoders), `CURLOPT_TIMEOUT_MS` 20000, a progress/xferinfo callback
  returning nonzero when `stopping_` — that is the cancellation path
  `stop()` relies on. `curl_global_init` once at startup (main.cpp),
  `curl_global_cleanup` at exit.
- Strategies ported faithfully: rutracker
  (`viewtopic.php?h=<hash>`) and nyaa (search by hash → parse view id →
  fetch `/view/<id>`), regexes translated QRegularExpression → PCRE2 (the
  filter_policy port is the in-repo precedent for exactly this
  translation; keep each regex source string byte-identical where PCRE2
  syntax allows — they are PCRE-compatible patterns already).
- All strategies for a hash run in parallel; fold into the single info
  JSON only once all finish, emit only if at least one succeeded (Qt
  contract). 1-h per-hash cooldown, cap 3 concurrent hashes (the
  cap-rationale comment about decompressed page memory applies at least as
  hard on the retro target — keep it).
- Output keys verbatim: `poster`, `description`, `contentCategory`,
  `trackers` (array of tracker names — this is what M5's allow/deny
  filters read), `rutrackerThreadId`, `nyaaThreadId`, `trackerName`.
  The TUI shows `description` (details pane) and `trackers`; `poster` is
  stored for parity/export but unused by a terminal UI.

### 3. `native/engine/tracker_service.{h,cpp}` (~80 loc)

Thin glue port: holds the two scrapers + the enabled flags,
`onTorrentIndexed(t)` → `checkCounts(t.hash)` + `checkInfo(t.hash,
t.name)`, `onCountsScraped` → index write, `onInfoScraped` → index write,
`stop()` = flags off first, then drain both scrapers (Qt's ordering
comment — keep it).

### 4. Index writes (`native/index/*`)

- **Extend `GroongaIndex::updateStats` to also stamp
  `trackers_checked = now`** — Qt's `updateTrackerCounts` sets
  `trackersChecked` and native's updateStats predates that; the column
  already exists (schema line ~297; Time column, seconds — upsert shows
  the ms→s conversion to copy). This is a behavior fix to bring the
  never-yet-called method up to its Qt counterpart, not a deviation.
- **New `GroongaIndex::mergeInfo(hash, const librats::Json& info)`** —
  port of repository mergeInfo: read the row's `info` object, shallow-merge
  the scraped keys over it, write back via a partial `load` (updateStats
  idiom). Add both to the `SearchIndex` interface next to `updateStats`.

### 5. Triggers + wiring (`native/engine/indexer.*`, `native/main.cpp`)

- Indexer gains an optional `onIndexed` callback (hash + name), invoked on
  every successful insert inside `handleDiscovered` — the single write
  path already funnels both crawler discoveries and peer inserts
  (peer_api's "Insertion (single write path)"), so one hook matches Qt's
  `torrentIndexed` coverage exactly.
- `EnginePipeline` grows `trackerService` (+ the two scrapers it owns).
  Construct whenever NodeHost is up; flags from config (item 6). Shutdown:
  `trackerService->stop()` before `nodeHost->stop()`.
- Works identically under `--console` (the crawler-driven trigger is the
  main volume source there).

### 6. Config (`native/platform/config.*`)

- `trackers` (Qt key name, default true): master gate for both halves —
  parity.
- `siteScraper` (native-only, default true): additionally gates the site
  half at runtime (deviation #2). `trackers=false` silences everything;
  `trackers=true, siteScraper=false` keeps cheap UDP counts with zero
  HTTP traffic.

### 7. TUI refresh (`native/tui/*`)

Port of the details-panel behavior (torrentdetailspanel.cpp):

- When the ResultView selection changes (details pane now showing a
  torrent), post `checkCounts(hash)`; the 5-min cooldown makes repeated
  cursor movement free. Site `checkInfo` only when the torrent's
  `info.trackers` is empty (Qt's have-info condition).
- When a scrape completes for the hash currently selected in the active
  tab, update that row's torrent in place (`ResultView` gains a small
  `updateSelectedStats(hash, ...)` — the native stand-in for Qt's
  `torrentUpdated` signal → panel reload) so the user actually sees counts
  go live. Marshal via screen Post as usual.
- Details pane: show `description` (truncated/wrapped) and the `trackers`
  list when present; show `trackers_checked` age next to seeders (e.g.
  "seeders: 12 (checked 3m ago)" — data is already in every search row).

## Deliberate deviations from Qt (all marked in comments)

1. **No mutexes / no queue poll timer** — EngineLoop confinement +
   drain-on-completion (translation, same effect).
2. **`siteScraper` runtime key** — Qt gates both scrapers under the single
   `trackers` key; the split follows the owner's optionality policy
   (runtime keys for non-essential CPU/disk-heavy features).
3. **libcurl / PCRE2** replace QNetworkAccessManager / QRegularExpression
   (forced translations; PCRE2 precedent is filter_policy).
4. **In-place selected-row refresh** instead of a torrentUpdated
   signal → full panel reload.

## Acceptance (DESIGN-native.md §12 M8, expanded)

- Fresh datadir, spider on: newly indexed torrents get non-zero
  seeders/leechers within ~a minute (log: scrape → updateStats), and
  `ratsn search <term> --json` shows a fresh `trackers_checked`.
- Select a well-seeded older torrent in Search/Top: counts visibly update
  in the details pane within the announce timeout; re-selecting inside
  5 min does not re-announce (cooldown log line).
- A known nyaa or rutracker torrent gains `description` +
  `trackers:["nyaa"]`-style identity in its info; an M5 tracker allow/deny
  filter then matches it in `ratsn cleanup --dry-run` on a datadir copy —
  the M5 gap closing end-to-end.
- `trackers=false`: no announces, no HTTP, indexing unaffected.
  `siteScraper=false`: counts still refresh, zero HTTP traffic
  (verify via ratsn-engine.log).
- Quit mid-burst (`q` and SIGTERM): shutdown completes promptly (≤ one
  announce/fetch), no crash — the stop/cancel-poll path working.
- TUI stays responsive during a crawl-driven scrape burst (pool-bounded,
  caps 5/3).

## Session workflow constraints (unchanged)

- **No local builds without asking first** — owner builds/tests; tarball
  flow after each commit.
- **All diagnostics through `platform::log()`** — the scrapers are
  bursty; keep per-scrape logging at the Qt files' qDebug-equivalent
  volume, not per-tracker chatter, or the log drowns (the M4 flood
  lesson).
- Verify the librats tracker.h / curl / PCRE2 API surfaces against real
  headers before writing against them.
- Independent of M7; either order, separate sessions, on the owner's go.
