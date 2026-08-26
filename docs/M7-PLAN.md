# M7 implementation plan — votes + feed (StorageManager), Feed tab

Written 2026-08-26 for the session implementing M7 (runs after M6; see
docs/M6-PLAN.md). Read alongside docs/DESIGN-native.md (§10 milestone row,
§12 acceptance). M7 is the "full parity target" milestone: torrent up/down
voting replicated across the swarm via librats' distributed key-value
StorageManager, and the ranked voted-torrents feed synced peer-to-peer —
the last of the Qt app's user-visible P2P features ratsn lacks.

The governing rule applies in full — **the Qt code is the behavioral spec:
port it, don't improve it.** The three services involved
(`p2p_store.cpp`, `voting_service.cpp`, `feed_service.cpp`) are small,
recently-refactored, and well-commented; the port is mostly mechanical.
Named deviations are listed under "Deliberate deviations".

## One correction to DESIGN-native.md first

§12's M7 acceptance line says "feed shows B's newly indexed torrents on A".
That does not match the code being ported: the feed is a **voted**-torrents
feed. The ONLY way a torrent enters the feed is a vote —
`application.cpp:206` connects `VotingService::votesUpdated` →
`feed->addByHash(hash)`, and nothing else calls `add`/`addByHash`. Feed
*sync* then spreads it peer-to-peer (`feed`/`feed_response` on connect).
The doc line is corrected in the same commit as this plan (code wins over
doc, per §12's own rule).

## Build prerequisite (owner's side, before the implementing session)

librats must be built with **`-DRATS_STORAGE=ON`** — it is a librats CMake
option, default OFF (`src/librats/CMakeLists.txt:97`); the Qt superbuild
forces it ON (root `CMakeLists.txt:146`). Without it there is no
`librats/storage/` in the build at all and votes can only degrade to
local-only counts (exactly Qt's `isAvailable() == false` path) — M7's
acceptance cannot pass. Concretely:

- The owner's MacPorts librats Portfile needs the flag added (same class of
  change as the existing static/shared patch).
- The define is `PUBLIC` on the exported `rats` target
  (`target_compile_definitions(rats PUBLIC RATS_STORAGE)`), so with
  `RATSN_USE_SYSTEM_LIBRATS` the `#ifdef RATS_STORAGE` guards in native
  code light up automatically — no ratsn-side flag.
- For the vendored-submodule fallback path, `native/CMakeLists.txt` must
  `set(RATS_STORAGE ON CACHE BOOL "" FORCE)` before `add_subdirectory`
  (mirror the root superbuild).
- Live-network note: shipped Qt clients are superbuild-built, so real peers
  on the mesh already replicate vote records — a single ratsn node against
  the live network can receive votes, but the clean acceptance check is two
  controlled nodes (below).

## Scope decisions

- **Feed tab** is the 5th TUI tab (Search / Top / Feed / Downloads /
  Status), reusing the shared `ResultView` exactly like the Top tab —
  feed items are plain torrents plus a `feedDate`.
- **Vote keys**: `v` = good, `V` = bad, on the selected result in any
  ResultView-backed tab (Search/Top/Feed). Voting requires the torrent to
  exist in the local index — Qt returns "Torrent not found" otherwise
  (voting_service.cpp:43); an un-indexed remote hit gets the same error in
  the status line. Details pane gains a `good: N   bad: M` line.
- **Feed persistence**: `<dataDir>/feed.json` (deviation #2 below — Qt
  stores the same JSON objects as rows of a Manticore `feed` table; the
  *shape* is identical, only the container differs).
- **`_torrent` in vote records is written, never read.** Qt embeds the full
  torrent JSON in each vote record "for peers that do not have this torrent
  yet" but no Qt code path consumes it on receive (`onRecordStored`
  aggregates counts only). Port that exactly: write it, ignore it. Do not
  invent an insert-from-vote path.
- No REST surface (deferred per §11), no vote UI beyond the keys above.

## Spec sources (read before writing code)

| What | Where |
|---|---|
| Record contract (`type` + injected `_key`/`_peerId`/`_timestamp`), why it must not change | `src/services/p2p_store.h` header comment |
| Store wrapper behavior: put metadata injection, find-by-prefix, change-callback marshalling | `src/services/p2p_store.cpp` |
| Vote lifecycle: key format `vote:{hash}:{peerId}`, record fields, dedup (in-memory set + store `has()`), aggregate (Wilson-free plain counts), local-column mirroring, remote-vote reaction | `src/services/voting_service.{h,cpp}` |
| Feed: ranking (`calculateScore` — port the formula digit-for-digit), maxSize 1000, add/refresh semantics, block filter (Bad/XXX), debounced 2s flush, wholesale `fromJsonArray` replace | `src/services/feed_service.{h,cpp}` |
| Feed item stored/wire shape (torrent JSON without files/info + `feedDate`) and WHY files are omitted | `feed_service.cpp` `itemToJson` comment |
| Wire handlers: `feed` request (payload ignored, replies full feed), `feed_response` replace-if-larger-or-newer + `insertFromPeer(..., trackReplication=false)`, on-connect feed pull | `src/peer/peer_api.cpp` handleFeedRequest/handleFeedResponse (~192/287), onPeerConnected (~358) |
| Service wiring + shutdown: votesUpdated→addByHash, `feed->load()` after transport start, `feed->save()` before transport stop | `src/app/application.cpp` ~131-136, ~206-208, ~243, ~277 |
| StorageManager attach point (subsystem, `<dataDir>/storage`) | `src/net/p2p_transport.cpp` ~310 |
| librats storage API: `put_json`/`get_json`/`has`/`keys_with_prefix`, `StorageChangeEvent`, `set_change_callback` (fires on a librats thread — must marshal), `set_sync_complete_callback`, `StorageConfig` | `src/librats/src/librats/storage/storage.h` |
| Torrent JSON for the `_torrent` payload / feed items | `native/domain/torrent_codec.h` `ToJsonOptions` (vote payload: files+info; feed items: neither) |

## Work items

### 1. NodeHost: attach the storage subsystem (`native/engine/node_host.*`)

Inside `#ifdef RATS_STORAGE`: `add_subsystem(std::make_unique<
librats::StorageManager>(sc))` with `sc.data_directory =
<dataDir>/storage`, added **before `node_->start()`** like every other
subsystem (M4's documented contract). New nullable accessor
`storage()` (returns nullptr when the ifdef is off or the node never
started). CMake per the prerequisite section above.

### 2. `native/engine/p2p_store.{h,cpp}` (~120 loc)

Port of `P2PStore`, minus Qt metatypes/signals:

- `put(key, librats::Json obj)`: inject `_key`, `_peerId` (from NodeHost —
  check `node.h` for the peer-id getter, same id PeerRegistry already
  logs), `_timestamp` (ms since epoch), then `storage->put_json`. On
  success, invoke the local-record listener directly (Qt emitted
  `recordStored(record, false)`).
- `find(prefix)`: `keys_with_prefix` + `get_json` per key → vector of a
  native `StoredRecord{key, type, data, peerId, timestamp}`.
- `has(key)`, `isAvailable()` (storage pointer non-null), `ourPeerId()`.
- Change callback: `set_change_callback` fires on a librats thread — parse
  the event, then **`EngineLoop::post`** the resulting StoredRecord to the
  listener (the native translation of Qt's queued-signal marshalling; same
  reason `Q_DECLARE_METATYPE` exists in the Qt header). Ignore non-remote
  events (locals are reported from `put`). Also port the
  sync-complete log line.
- Listener registration: a single `std::function<void(const StoredRecord&,
  bool isRemote)>` per consumer need (VotingService is the only one) — no
  generic signal bus.
- Everything wrapped in `#ifdef RATS_STORAGE` with graceful degraded
  behavior compiled otherwise, mirroring the Qt file's structure.

### 3. `native/engine/voting.{h,cpp}` (~180 loc)

Port of `VotingService`, callback-based instead of Qt signals:

- `vote(hash, isGood, ResultCallback)`: exact Qt sequence — 40-hex check,
  torrent lookup (fail "Torrent not found"), already-voted early return
  (in-memory `selfVotedHashes_` OR store `has("vote:{hash}:{peerId}")`)
  with current counts + `alreadyVoted=true`, else store the record, bump
  the local mirror, insert into `selfVotedHashes_`, prefer the distributed
  aggregate for returned counts, fire `votesUpdated`.
- Vote record: `{type:"vote", torrentHash, vote:"good"|"bad",
  _index:"vote:{hash}", _torrent:<codec toJson files+info>}` under key
  `vote:{hash}:{peerId}` — copy field names/values verbatim; this shape is
  replicated across the live swarm and must aggregate with Qt peers'
  records.
- `getVotes(hash, cb)`: distributed aggregate when available, else local
  columns, else zeros — port the `source` field values too (the TUI status
  line can show them).
- `aggregate(hash)`: `find("vote:{hash}:")`, count `vote == "good"/"bad"`,
  `selfVoted` on peer-id match.
- Remote record reaction (`onRecordStored`): `type == "vote"` + 40-hex
  torrentHash → re-aggregate → mirror onto local columns if drifted → fire
  `votesUpdated`.
- Local-column mirroring: Qt rewrites the whole torrent
  (`repository_->update`); native adds a narrow
  `GroongaIndex::updateVotes(hash, good, bad)` — a partial `load` exactly
  like the existing `updateStats` (groonga_index.cpp:435). Add it to the
  `SearchIndex` interface next to `updateStats` (Indexer-independent, but
  VotingService holds the abstract reference like Indexer does).
- `votesUpdated` → callback; main.cpp wires it to `feed.addByHash`
  (application.cpp:206's connect).

### 4. `native/engine/feed.{h,cpp}` (~250 loc)

Port of `FeedService`, EngineLoop-confined:

- `FeedItem{domain::Torrent, int64_t feedDate /*seconds*/}`;
  `itemToJson` = codec `toJson` with `includeFiles=false,
  includeInfo=false` + `feedDate` (keep the Qt comment explaining the
  omission); `itemFromJson` symmetric.
- `add`: block filter first (`ContentType::Bad` or `ContentCategory::XXX`
  never enter — port `shouldBlock`), existing items refresh
  good/bad/seeders but keep their original `feedDate`, new items get
  now-seconds, maxSize-1000 eviction loop ported as-is, then `reorder()` +
  `feedDate_ = now` + markDirty + feedUpdated.
- `calculateScore`: port digit-for-digit — `maxTime = 600000` seconds age
  cap, `relativeTime²  + good·1.5·rt − bad·0.6·rt + wilson(good,bad)` with
  z = 1.96. Keep the decorate-sort-undecorate `reorder()` (its comment
  explains the strict-weak-ordering hazard — real).
- `fromJsonArray(array, remoteFeedDate)`: wholesale replace with block
  filtering and the maxSize cap, port exactly.
- Persistence: debounced — `markDirty` starts a one-shot 2s
  `EngineLoop::postDelayed` flush (guard against double-scheduling with a
  bool, the native translation of the single-shot QTimer); `save()` =
  flush-if-dirty; shutdown calls it explicitly (item 6). `persistNow` →
  `<dataDir>/feed.json`, write-temp-then-rename (reuse the
  session_store.cpp idiom; consider a tiny shared helper). `load()` filters
  blocked content and recomputes `feedDate_` = max item date.
- TUI coupling: a `revision()` counter bumped on every mutation (the
  established native substitute for `feedUpdated`), plus the direct
  callback main.cpp uses if needed.

### 5. PeerApi: real feed handlers (`native/engine/peer_api.*`)

Replace the M4 stubs (marked `TODO(M6)` in peer_api.cpp:237 — they meant
this milestone):

- `handleFeedRequest`: ignore the request payload (Qt does), reply
  `feed_response {feed: toJsonArray(0, size), feedDate, size}`.
- `handleFeedResponse`: `replace = remoteSize > localSize || (remoteSize ==
  localSize && remoteFeedDate > localFeedDate)`; on replace,
  `fromJsonArray` then `insertFromPeer(item, /*trackReplication*/ false)`
  for every item — the existing native `insertFromPeer` already has the
  bool and the Indexer funnel; feed inserts deliberately don't count toward
  replication stats (Qt comment, keep it).
- `onPeerConnected`: send `feed {localSize, localFeedDate}` before the
  existing replication ask (Qt's order: feed pull first,
  peer_api.cpp:367-374).
- PeerApi gains a borrowed nullable `FeedService*` (null → keep answering
  the empty stub shape, so a spider-only/no-feed configuration still
  responds).

### 6. Wiring + shutdown (`native/main.cpp`)

- `EnginePipeline` grows `p2pStore`/`voting`/`feed`. Construct after
  `NodeHost::start()` succeeds (storage pointer exists from attach time;
  callbacks are safe to set post-start — note the Qt app sets them in the
  P2PStore constructor, but its transport is started first).
- Wire `votesUpdated` → `feed->addByHash` (application.cpp:206).
- `feed->load()` after the pipeline starts, next to the M6
  `downloads->loadSession` call (application.cpp:243's slot).
- Shutdown: `feed->save()` before `nodeHost->stop()` (application.cpp:277's
  ordering), alongside the existing downloads save.
- `--console` gets votes/feed passively (storage syncs, feed serves peers);
  no console-side commands. Everything degrades cleanly when
  `RATS_STORAGE` is off or the node is down: votes fall back to local
  columns (Qt behavior), feed still works locally + over the wire (it does
  not depend on storage).

### 7. TUI (`native/tui/*`)

- `feed_tab.{h,cpp}`: clone of TopTab's shape — ResultView + a
  reload-on-activation hook in app.cpp's per-frame tab check, plus reload
  when `feed->revision()` changed (poll it in the same place; the Top tab's
  activation check already runs per-frame). Rows show the standard result
  line; details pane shows `feedDate` as a plain date line.
- Tab count 3 → 5 was already generalized in M6 (modulo-kTabCount); bump
  `kTabCount`, add the tab-bar label.
- ResultView: `v`/`V` keys → post `voting->vote(hash, good, cb)` to the
  EngineLoop; the callback marshals back (screen_.Post, the
  handleSaveTorrent idiom) into `statusMessage_`: "voted: N good / M bad",
  "already voted (N good / M bad)", or the error. Details pane: `good`/
  `bad` counts line (fields already come back from every search — they are
  in `kOutputColumns`).
- No new key conflicts: ResultView now owns m/t/d/v/V; the app-level hint
  line gains "v: vote".

## Deliberate deviations from Qt (all marked in comments)

1. **No mutex / signal layer** — EngineLoop confinement + callbacks, same
   translation as every prior milestone. The only cross-thread edge is the
   storage change callback (item 2), marshalled with `EngineLoop::post`.
2. **Feed persists to `<dataDir>/feed.json`**, not a search-engine table.
   Identical item JSON, write-temp-rename. (Qt's FeedRepository is
   deliberately dumb opaque-JSON storage anyway — its own header says so.)
3. **`revision()` counter instead of `feedUpdated` signal** for the TUI
   (established M6 pattern).

## Acceptance (DESIGN-native.md §12 M7, expanded — needs two nodes)

Two ratsn instances with distinct datadirs (`--connect host:port` from M4
makes the pairing deterministic), or one ratsn against a Qt client:

- Vote `v` on an indexed torrent on node A: status line shows counts;
  `getVotes`-backed details on **B** show the same counts after storage
  sync (watch `[P2PStore]`-equivalent log lines in ratsn-engine.log).
- The voted torrent appears in A's Feed tab immediately, and in B's feed
  after B reconnects (feed pull on connect) or votes sync — B's smaller/
  older feed is replaced per the size/date rule.
- Restart A: Feed tab repopulates from feed.json; ranking order sane
  (fresh + upvoted items on top).
- Vote again on the same torrent: "already voted", counts unchanged.
- Vote on a Bad/XXX-classified torrent: counts update but it never enters
  the feed (block filter).
- `V` (bad) decrements nothing but increments `bad` — verify both columns
  mirror into search output (`ratsn search <term> --json` shows good/bad).
- Storage-less build (librats without RATS_STORAGE): `v` still bumps local
  counts and reports them, nothing replicates, nothing crashes.

## Session workflow constraints (unchanged)

- **No local builds without asking first** — owner builds/tests; tarball
  flow after each commit (`git archive --prefix=rats-search-<short-sha>/
  --format=tar.gz -o out.tar.gz HEAD`, SendUserFile).
- **All diagnostics through `platform::log()`** — never bare
  std::cout/std::cerr (M4 lesson; FTXUI renders via std::cout).
- Verify FTXUI/librats API surfaces against the real headers (v7.0.3 via
  raw.githubusercontent.com where needed) before writing against them —
  the storage header especially, since no native code has touched it yet.
- M7 and M8 are independent (M8 touches scrapers/index-stats only); either
  order, separate sessions, each starts when the owner says so.
