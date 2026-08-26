# rats-native — a Qt-free rats-search on librats

Design for a from-scratch, dependency-minimal reimplementation of rats-search's
primary functionality directly on librats. Working name **rats-native**, binary
`ratsn` (placeholder). Target: plain C++17 that builds anywhere gcc does —
including 32-bit big-endian PowerPC (OS X 10.6, gcc16) — with no Qt, no
Manticore, no external daemon.

Status: design, ready for implementation. The Groonga smoke test
(schema/load/CJK search/persistence across reopen) has already passed on the
32-bit BE target. §12 contains handoff notes for the implementing session.

---

## 1. Goals and non-goals

**Goals (primary functionality parity):**
- DHT crawling/spider → BEP9 metadata fetch → classify/filter → index.
- Local full-text search over torrent names (file-path search where the
  platform allows), with size/seeder/category filters and sorting.
- P2P search against other rats clients, **wire-compatible with the existing
  network** (protocol `rats-search/3`, message schemas verbatim).
- Torrent download via the librats BitTorrent client, with session resume.
- Votes/feed/top via the replicated librats store (later milestone).
- Console (headless) mode and an interactive TUI (FTXUI).

**Non-goals (v1):**
- REST/WebSocket API (the TUI is in-process; API can come later).
- Tracker-site scraping (rutracker/nyaa descriptions), update checks — these
  need HTTPS and are optional features, not primary functionality.
- GUI, translations, legacy v1 migration.

**Hard constraints:**
- C++17, CMake, no exceptions to portability: every dependency must build on
  32-bit BE PPC with a modern gcc.
- Endianness-clean by construction: never memcpy structs to wire/disk; all
  wire I/O goes through librats (which owns the BT/DHT formats) or Groonga
  (which owns its storage); app code sticks to JSON strings. This is a
  standing coding requirement, not a test gate: development and all milestone
  checks run on little-endian, BE testing happens opportunistically whenever
  the target box is convenient, and a BE bug found there is fixed like any
  other bug.

## 2. What librats provides vs. what we build

| Layer | Provided by librats | We write |
|---|---|---|
| Transport, Noise crypto, identity | `Node` | config only |
| DHT (Mainline, spider), mDNS, PEX, NAT | `DhtDiscovery`, `MdnsDiscovery`, `PeerExchange`, `PortMappingService`, `HolePunch`, `ReconnectionService` | wiring |
| BitTorrent client + BEP9 metadata | `Bittorrent` subsystem (`bittorrent/client.h`) | download orchestration + session file |
| Peer JSON messaging | `MessageJson` subsystem | the rats-search message handlers |
| Replicated LWW KV store | `StorageManager` (`RATS_STORAGE`) | votes/feed record logic |
| JSON, logging, fs | `librats::Json`, `librats::Logger`, fs utils | — |
| Full-text index | — | Groonga embedding (`SearchIndex`) |
| Domain logic | — | Torrent model, codec, classifier, filters |
| UI | — | FTXUI TUI + console mode |

The reference implementation for all "we write" logic is the existing Qt core
(`src/domain`, `src/net/crawler.cpp`, `src/peer/peer_api.cpp`,
`src/services/*`): port the logic, drop the Qt types. This is a
transliteration, not a redesign — the schemas and behavior are already proven.

## 3. Process and threading model

Single process, three kinds of threads:

```
librats internal threads          EngineLoop (1 thread)              main thread
(reactors, DHT, BT, gossip)       ─ owns ALL app state ─             (TUI)
        │  callbacks                │                                   │
        └──── post(closure) ──────► │  task queue + timer wheel         │
                                    │  crawler bookkeeping              │
                                    │  Groonga ctx (writes+reads)       │
                                    │  peer message handlers            │
                                    │  download progress fan-out        │
                                    └──── ScreenInteractive::Post ────► │ redraw
                                    ◄──── post(search task) ────────────┘
```

- **EngineLoop** is a hand-rolled single-threaded executor (~150 lines):
  `post(fn)`, `postDelayed(fn, ms)` on a mutex+condvar queue with a timer
  wheel. It replaces the Qt event loop + the 74 QTimers. Everything the Qt app
  ran on its main thread runs here; no other thread touches app state. This is
  the simplest correctness story and costs nothing on the single-core G4.
- librats callbacks (peer connected, message received, metadata fetched,
  download progress) do nothing but `engine.post(...)`.
- **Groonga**: one `grn_ctx` owned by the EngineLoop thread. `grn_ctx` is not
  thread-safe; confining it removes the question. Queries are milliseconds at
  this scale — reads do not need their own thread in v1 (a read-only second
  ctx is a later optimization).
- **TUI** runs FTXUI's `ScreenInteractive` on the main thread. UI → engine is
  `engine.post`; engine → UI is `screen.Post([=]{ model.update(...); })`,
  which is FTXUI's documented thread-safe path. `--console` skips FTXUI
  entirely and the main thread just joins the EngineLoop (mirrors the current
  `--console` design).

## 4. Module breakdown

```
native/
  platform/   config.{h,cpp}        rats.json load/save (librats::Json)     ~200 loc
              paths.{h,cpp}         data dir resolution, --data-dir          ~100
              single_instance.*     lockfile + unix-socket ping              ~150
              engine_loop.*         executor + timers                        ~150
  domain/     torrent.h             Torrent/File/SearchHit (std types)       ~120
              torrent_codec.*       JSON codec — field names VERBATIM        ~250
              classifier.*          port of content_classifier               ~200
              filter_policy.*       size/count/regex/adult filters (PCRE2)   ~200
  index/      search_index.h        backend interface (~10 methods)          ~80
              groonga_index.*       schema DDL, queries, upserts             ~700
  engine/     node_host.*           librats Node + subsystem wiring          ~300
              crawler.*             spider + metadata queue (port)           ~500
              indexer.*             dedup → classify → filter → insert       ~250
              peer_api.*            wire handlers + remote search fan-out    ~600
              downloads.*           BT client wrapper + downloads.json       ~450
              store.*               votes/feed on StorageManager (M6)        ~500
  tui/        app.*  search_tab.*  downloads_tab.* top_tab.* status_bar.*   ~1800
  main.cpp    CLI parsing, mode select, signal handling                      ~200
                                                              total  ≈ 6.7k loc
```

## 5. Search index (Groonga, embedded)

Use **libgroonga's C API in-process** (`grn_init`, `grn_ctx_open`,
`grn_ctx_send/recv` with the same command strings as the CLI) — not the
daemon. The validated smoke-test schema generalizes to:

```
table_create  Torrents TABLE_HASH_KEY ShortText            # _key = infohash
column_create Torrents name COLUMN_SCALAR ShortText
column_create Torrents size COLUMN_SCALAR UInt64
column_create Torrents files COLUMN_SCALAR UInt32
column_create Torrents piece_length COLUMN_SCALAR UInt32
column_create Torrents added COLUMN_SCALAR Time
column_create Torrents content_type COLUMN_SCALAR UInt8
column_create Torrents content_category COLUMN_SCALAR UInt8
column_create Torrents seeders COLUMN_SCALAR UInt32
column_create Torrents leechers COLUMN_SCALAR UInt32
column_create Torrents completed COLUMN_SCALAR UInt32
column_create Torrents good COLUMN_SCALAR UInt32
column_create Torrents bad COLUMN_SCALAR UInt32
column_create Torrents trackers_checked COLUMN_SCALAR Time
column_create Torrents info COLUMN_SCALAR Text             # JSON blob (poster, files list, extras)
table_create  Terms TABLE_PAT_KEY ShortText --default_tokenizer TokenBigram --normalizer NormalizerAuto
column_create Terms idx_name COLUMN_INDEX|WITH_POSITION Torrents name

# optional, gated by config `fileIndex` (default OFF on 32-bit):
table_create  Files TABLE_NO_KEY
column_create Files torrent COLUMN_SCALAR Torrents
column_create Files path COLUMN_SCALAR ShortText
column_create Files size COLUMN_SCALAR UInt64
column_create Terms idx_path COLUMN_INDEX|WITH_POSITION Files path
```

- **Queries** map 1:1 to the current SphinxQL surface: `--match_columns name
  --query …` for MATCH, `--filter 'size >= X && seeders > Y &&
  content_category != N'` for attribute filters, `--sort_keys
  -seeders|-added|-_score`, `--offset/--limit` for paging. Top torrents =
  filter + `-seeders`. File search queries `Files` and joins back via the
  `torrent` reference column.
- **Random torrents** (wire message `randomTorrents`): Groonga has no
  ORDER-BY-RAND; sample random `_id`s in `[1, max_id]` and fetch the ones that
  exist. Good enough for the feature's purpose.
- **Snippet/highlight**: done app-side (trivial substring/term markup) — no
  engine dependency, mirrors what `SNIPPET()` produced.
- **32-bit budget**: Groonga's storage is mmap-based, so address space caps
  the DB (~4 GB practical) on 32-bit. Defaults there: `fileIndex=false` (file
  lists still *stored* in `info` for display, just not FT-indexed). On 64-bit
  builds `fileIndex=true` matches current behavior.
- `SearchIndex` stays a narrow interface (upsert, remove, searchNames,
  searchFiles, top, random, updateStats, counts) so an SQLite-FTS5 backend can
  be added later without touching the engine.

## 6. Wire compatibility contract (MUST NOT DRIFT)

- librats `NodeConfig.protocol = "rats-search/3"` — verbatim. The protocol id
  is bound into the Noise handshake; matching it *is* joining the network.
- Message types over `MessageJson`, names verbatim from `peer/peer_api.cpp`:

| Request | Response | Purpose |
|---|---|---|
| `searchTorrent` | `searchTorrent_response` | remote name search |
| `searchFiles` | `searchFiles_response` | remote file search |
| `topTorrents` | (response w/ `torrents`) | top list exchange |
| `randomTorrents` | `randomTorrents_response` | random sampling |
| `feed` | `feed_response` | activity feed |
| `torrent` | `torrent_response` | fetch full record by hash |
| `torrentAnnounce` | — | broadcast newly indexed hash |
| `client_info` | — | version/capabilities hello |

- Torrent JSON schema: port `domain/torrent_codec.cpp` field-for-field
  (`hash, name, size, files, filesList/files_list, pieceLength, added,
  contentType, contentCategory, seeders, leechers, completed, trackersChecked,
  good, bad, info, path, peer, remote, isFileMatch/fileMatch, matchingPaths,
  info_hash`). Keep both spellings where the codec accepts both.
- Store records (votes/feed): the librats `StorageManager` record contract —
  a `type` field plus injected `_key`/`_peerId`/`_timestamp` — and the key
  format are replicated across the swarm; copy them exactly (see
  `services/p2p_store.h` commentary).
- **Golden-file tests**: capture real JSON messages from the current Qt app
  (two local instances) into fixtures; the new codec must round-trip them
  byte-compatibly (field set, not ordering). This pins compatibility better
  than any code review.

## 7. TUI (FTXUI)

FTXUI fits: C++17, MIT, CMake-friendly static lib, interactive component
model (Input, Menu, Tabs, CatchEvent), renders over plain ANSI (works in 10.6
Terminal.app; stick to the 256-color palette, no truecolor), and
`ScreenInteractive::Post` gives the thread-safe engine→UI channel. Use it.

Layout (mirrors the Qt main window):

```
┌ rats-native ────────────────────────────────────────────────────────────┐
│ [ Search ] [ Downloads ] [ Top ] [ Feed ] [ Status ]                    │
│ ┌─────────────────────────────────────────────────────────────────────┐ │
│ │ > ubuntu 24.04_                                    [x] files  sort▾ │ │
│ ├───────────────────────────────┬─────────────────────────────────────┤ │
│ │ Name              Size  Seeds │ Ubuntu 24.04 Desktop amd64 ISO      │ │
│ │ ▸ Ubuntu 24.04…   5.8G  1234  │ 5.8 GB · 12 files · added 2026-…    │ │
│ │   ubuntu-24.04…   5.8G   987  │ video/distro · ▲34 ▼2               │ │
│ │   Ubuntu Serve…   2.6G   401  │─────────────────────────────────────│ │
│ │   …                           │ casper/vmlinuz            8.4 MB    │ │
│ │                               │ casper/initrd            64.1 MB    │ │
│ │                               │ …                                   │ │
│ │                               │ [d]ownload  [m]agnet  [v]ote        │ │
│ ├───────────────────────────────┴─────────────────────────────────────┤ │
│ │ peers 14 · dht 1.2M · indexed 84,312 (+3/s) · ↓ 2 active            │ │
│ └─────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────┘
```

- **Search tab**: `Input` (search-as-you-type debounced 300 ms on the engine
  timer), results as a selectable list (custom Component, windowed render for
  large result sets), details pane from the selected row; remote results
  merge in asynchronously tagged `[peer]` (dedup by hash, local first —
  same merge rule as today's search service).
- **Downloads tab**: list with progress gauges (`ftxui::gauge`), keys:
  pause/resume/remove.
- **Top/Feed tabs**: read-only lists; Feed updates live from
  `torrentAnnounce`/store events.
- **Status tab**: node id, listen ports, NAT status, subsystem states, index
  stats; doubles as the settings view for the few runtime-togglable options
  (spider on/off, filters).
- Keys: `Tab` cycle tabs, `/` focus search, `j/k`/arrows navigate, `Enter`
  details, `d` download, `m` copy magnet (print to a copy-buffer line —
  no clipboard dependency), `q` quit (with confirm if downloads active).
- Redraw discipline for the G4: engine batches UI posts (coalesce index-count
  ticks to 1/s; never post per-torrent).

Console mode (`--console`) is the same engine with a log line instead of a
screen; also `ratsn search "query"` as a one-shot CLI (start node? no — query
the local index only, for scripting).

## 8. Config and data directory

`<datadir>/rats-native.json`, flat JSON, reusing the current key names where
the concept survives (`spider`, `walkInterval`, `dhtPort`, `upnp`,
`holePunch`, `downloadPath`, `trackers`, `filters{sizeMin,sizeMax,maxFiles,
namingRegExp,namingRegExpNegative,adultFilter,contentType}`, `safeSearch`)
plus `fileIndex` (see §5). Separate files, as today: `downloads.json`
(session resume), Groonga DB under `<datadir>/index/`. Default datadir:
`~/.rats-native` (10.6-friendly), `--data-dir` overrides. Reusing key names
keeps a future "import settings from rats-search" trivial.

## 9. Build and dependencies

- CMake ≥ 3.16, C++17. Options: `RATSN_WITH_TUI` (default ON),
  `RATSN_FILE_INDEX_DEFAULT` (OFF on 32-bit), `RATS_STORAGE` passthrough.
- Dependencies, all verified-portable:
  - **librats** — git submodule (shared with the parent repo's checkout).
  - **groonga** — system library via pkg-config (a MacPorts port exists and
    is maintained by the project owner). If building it from source, the
    verified minimal configure is:
    `git submodule update --init --depth 1 vendor/onigmo-source` (mandatory),
    then `-DGRN_WITH_MRUBY=OFF -DGRN_WITH_APACHE_ARROW=OFF
    -DGRN_WITH_SIMSIMD=OFF -DGRN_WITH_DOC=OFF -DGRN_WITH_EXAMPLES=OFF
    -DGRN_WITH_BENCHMARKS=OFF -DGRN_WITH_TOOLS=OFF -DGRN_WITH_OPENZL=no
    -DGRN_WITH_LLAMA_CPP=no -DGRN_WITH_SIMDJSON=no -DGRN_WITH_ZSTD=no
    -DGRN_WITH_ZLIB=no -DGRN_WITH_MECAB=no` (dep gates are strings, not
    booleans; needs CMake ≥ 3.22; old gcc may need
    `-Wno-error=incompatible-pointer-types`).
  - **FTXUI** — system library via CMake's `find_package(ftxui 7 REQUIRED
    CONFIG)` (the config FTXUI's own install generates), not vendored. Pinned
    to the v7.x line specifically: v7.0.3 renamed `ScreenInteractive`'s
    underlying class to `App` (`ScreenInteractive` is now just an alias) and
    reworked parts of the component API relative to the v5.0.0 baseline
    originally planned here; `native/tui/` is written against v7.0.3's API,
    not v5's. On the retro target it is provided by the project owner's own
    ports overlay (macos-powerpc/powerpc-ports, `devel/FTXUI`).
  - **PCRE2** — system or vendored; used only by `filter_policy` (user
    regexes need real PCRE semantics; `std::regex` is the build-time
    fallback with documented semantic loss).
- No exceptions across the librats callback boundary; engine code may use
  them internally.

## 10. Milestones

| M | Deliverable | Proves | ~loc |
|---|---|---|---|
| M1 | skeleton: EngineLoop, config, GroongaIndex, console mode, `ratsn search` against a hand-loaded index | index layer | 2.0k |
| M2 | crawler + indexer live: spider → classify → filter → searchable; stats line | the pipeline | 1.2k |
| M3 | TUI: search/status tabs on the live index | usability | 1.5k |
| M4 | wire compat: peer_api handlers + remote search merge; golden-file tests against the Qt app | network membership | 1.0k |
| M5 | search quality + data management: strict match (default on), filter panel, tracker filters, export/import, index size cap + cleanup, Top tab, .torrent save | daily usability | 1.7k |
| M6 | downloads tab + session resume | BT client integration | 0.8k |
| M7 | votes/feed via StorageManager; feed tab | full parity target | 1.2k |
| M8 | tracker scrapers: swarm stats (seeders/leechers) + site metadata (poster/description, tracker identity) | stats quality | 1.0k |
| M9 | optional GUI: borealis/nanovg front-end (SDL2 or GLFW backend) over the same engine; search/top/feed/downloads views, poster browse | front-end replaceability | 2.5k |

M4's concrete implementation plan (scope decisions, module list, wire surface,
acceptance-lab setup) is in docs/M4-PLAN.md. M5's is in docs/M5-PLAN.md and
M6's in docs/M6-PLAN.md (both scoped 2026-08-25; downloads and votes/feed
were renumbered M6/M7 then). M5 and M6 are executed by separate sessions, in
that order. M7's plan is docs/M7-PLAN.md and M8's docs/M8-PLAN.md (both
scoped 2026-08-26); M7 and M8 are independent of each other and can run in
either order, each in its own session. M7 requires librats built with
-DRATS_STORAGE=ON; M8 makes libcurl a hard dependency (see M8-PLAN's
dependency-policy note).

M9 (optional GUI, registered 2026-08-26) deliberately has no plan doc yet.
Its **toolkit decision is settled and not to be relitigated**: borealis
(xfangfang's fork) + nanovg + yoga, over SDL2 on the retro targets (the
owner already runs this stack on PPC/BE — xfangfang/wiliwili#570, including
the big-endian color fixes) or GLFW on newer systems (note: mainline GLFW
has no IME support at all). Qt4 and Cocoa were considered and rejected
(dead-toolkit weight / ObjC++); upstream Qt6 `src/ui/` is not reusable
under any toolkit — UX blueprint only. Architecturally the GUI is a second
shell beside the TUI: a `gui::run()` over the same EnginePipeline pointer
surface, gated by its own build option (`RATSN_WITH_GUI`), marshalling with
a post-to-main-thread helper exactly like the TUI's `screen_.Post` idiom;
`native/engine|index|platform` stay front-end-free. Write docs/M9-PLAN.md
in a dedicated session once: (a) a small SDL text-input probe has run on
the retro target (log SDL_TEXTINPUT/SDL_TEXTEDITING under a Cyrillic
layout, a CJK IME, and clipboard paste — paste is the accepted fallback
for typed CJK), (b) the exact borealis fork/tag proven in the owner's
wiliwili port is pinned, and (c) M7/M8 have landed, since the GUI renders
their features (feed, votes, posters).

All development and milestone checks run on little-endian — it's simpler for
technical reasons and nothing here depends on the target hardware. Big-endian
runs (librats examples across an LE↔BE pair, then the app itself) can happen
at any point when the box is convenient; whatever they surface is fixed as a
normal bug, upstream in librats if that's where it lives.

## 11. Risks and open questions

- **librats on big-endian** — never run there; BT/DHT wire formats are BE but
  host code has only seen LE. Not a gate: fix bugs when a BE run surfaces
  them (upstream in librats where applicable).
- **Groonga 32-bit ceiling** — mitigated by `fileIndex=false`; name-only
  index for ~10M torrents fits comfortably under 4 GB. Revisit if not.
- **Crawl rate on period hardware** — G4 CPU + crypto (Noise per connection)
  will cap peer counts; expose `maxPeers`/queue depths in config; the
  pipeline is pull-based so backpressure is natural.
- **FTXUI redraw cost on slow terminals** — coalesced posts (§7); worst case,
  drop to a plain line-mode UI which the console path already provides.
- **Schema drift vs. the living network** — golden-file tests in CI (M4) and
  the rule that `torrent_codec` field names are copied, never "cleaned up".
- **Open**: whether v1 ships a read-only REST endpoint for remote TUI use
  (deferred; single-machine TUI covers the stated need).

## 12. Handoff notes for the implementing session

**Where the truth lives.** This document defines architecture and contracts;
behavior is specified by the existing code. When this doc and the existing
code disagree on observable behavior (message payloads, filter semantics,
classifier output), the existing code wins — port it, don't improve it.

| Question | Spec source (in this repo) |
|---|---|
| Wire message payloads, handler behavior | `src/peer/peer_api.cpp` (+ `src/net/p2p_transport.cpp` for Node/subsystem wiring, protocol id at line ~240) |
| Torrent JSON schema | `src/domain/torrent_codec.cpp` |
| Classifier / category rules | `src/domain/content_classifier.cpp` |
| Filter semantics | `src/services/filter_policy.cpp` |
| Crawler behavior (spider, metadata queue, rate limits) | `src/net/crawler.cpp` |
| Download/session behavior | `src/services/download_service.cpp`, `torrent_session_store.cpp` |
| Votes/feed record contract | `src/services/p2p_store.h` (header comment), `voting_service.cpp`, `feed_service.cpp` |
| librats API | headers under `src/librats/src/librats/` (`node/`, `subsystems/`, `bittorrent/`, `storage/`, `util/json.h`) |
| Search query surface to replicate | `src/data/query.cpp`, `src/data/torrent_repository.cpp` |

**Repo layout & bootstrap.** New code goes in a top-level `native/` directory
with its own standalone `CMakeLists.txt` (do not touch the root Qt build).
It consumes librats via the existing `src/librats` submodule
(`git submodule update --init --depth 1 src/librats`), finds groonga + pcre2
via pkg-config, and finds FTXUI (v7, system-installed) via
`find_package(ftxui CONFIG)` -- see §9, not vendored. First target to bring
up: `ratsn --console` that starts an empty EngineLoop and exits cleanly on
SIGINT.

**Groonga embedding bootstrap.** `grn_init()` once per process;
`grn_ctx *ctx = grn_ctx_open(0)`; create/open the DB with
`grn_db_create/grn_db_open` on `<datadir>/index/db`; drive everything else
through `grn_ctx_send(ctx, cmd, len, 0)` + `grn_ctx_recv` using the exact
command strings from §5 (same strings the CLI smoke test used — keep a
`groonga` CLI handy to debug the DB out-of-process). One ctx, EngineLoop
thread only. `grn_fin()` on shutdown.

**Milestone acceptance checks** (all on little-endian; see §1 on endianness).
- M1: `ratsn search ubuntu` returns hand-loaded rows; kill -9 + rerun still
  returns them.
- M2: fresh datadir, spider on: index count grows without intervention;
  `ratsn search <common term>` hits within minutes; filters demonstrably drop
  (log line) rejected torrents.
- M3: TUI usable over plain ssh + `TERM=xterm-256color`; search-as-you-type;
  no flicker at 1 Hz stat updates.
- M4: against a running Qt rats-search on localhost: remote search returns its
  results tagged remote; golden fixtures captured from that instance
  round-trip through the new codec.
- M5: strict search returns only full-word matches (locally and for remote
  hits); filter panel narrows results live; export → import into a fresh
  datadir round-trips; `ratsn cleanup` retro-applies tightened filters;
  `indexMaxTorrents` holds the index at the cap under live inflow; Top tab
  lists by seeders; `t` writes a .torrent a real client opens. Details:
  docs/M5-PLAN.md.
- M6: magnet added in TUI downloads to completion; restart resumes an
  unfinished download.
- M7: vote cast on node A visible on node B; feed shows B's newly voted
  torrents on A (the feed is the voted-torrents feed — entries enter only
  via votes, then sync peer-to-peer; see M7-PLAN's correction note).
- M8: seeders/leechers on a self-crawled torrent refresh from tracker
  announces; a known-tracker torrent gains poster/description + tracker
  identity.
- M9: with RATSN_WITH_GUI on, every TUI flow works in the GUI
  (search-as-you-type including CJK-by-paste, download to completion, feed
  browse, vote); M8 posters render in a browse view; runs on the PPC
  target; the TUI build is unaffected when the option is off.

**Sequencing rule.** Milestones are strictly ordered; do not start M(n+1)
until M(n)'s check passes. All checks run on a modern little-endian platform;
BE/PPC validation is independent of the milestone sequence and never blocks
it.
