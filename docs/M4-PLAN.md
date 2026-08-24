# M4 implementation plan — wire compat (peer_api + remote search + replication)

Written 2026-08-24 for the session implementing M4. Read alongside
docs/DESIGN-native.md (§10 milestone row, §12 acceptance + spec-source table).
The governing rule is unchanged: **the existing Qt code is the behavioral
spec — port it, don't improve it.** Wire message-type names and JSON field
names are copied verbatim (deployed peers must keep interoperating).

## Goal and acceptance

ratsn becomes a member of the rats-search peer mesh: it answers other peers'
requests, consumes their responses through the one Indexer write path, fans
searches out to peers, and pulls replication. Acceptance (DESIGN-native.md
§12): against a running Qt rats-search on localhost, a ratsn search returns
that instance's results tagged remote; golden fixtures captured from that
instance round-trip through the native codec.

## Scope decision: replication is IN

The original §10 row is "peer_api handlers + remote search merge". This plan
*adds the replication ask-loop* (port of `src/services/replication_service.cpp`,
~80 lines of timer policy). Rationale, from the 2026-08-24 review session: the
DHT spider cold-starts at ~2 torrents/30min and that is expected — the Qt app's
actual index growth is dominated by replication (5 random torrents per
connected peer every 5–10 s, adaptive up to 60 s). Without it, M4 would ship
mesh membership but leave the headline slow-growth problem unsolved. The
handlers (`randomTorrents`/`randomTorrents_response`) are peer_api scope
anyway; only the periodic asking policy is extra.

Out of scope, unchanged: downloads (M5); votes, real feed content, and
StorageManager (M6). The `feed`/`feed_response` message pair still gets
*handlers* in M4 (peers will send them): answer `feed` with an empty feed
response, consume `feed_response` as a no-op. Leave a TODO(M6) at both.

## Spec sources (read before writing code)

| What | Where |
|---|---|
| Handler behavior, payload shapes, reply policy | `src/peer/peer_api.cpp` (the whole file; it is the wire spec) |
| Handshake (client_info), peer stats | `src/services/peer_registry.h/.cpp` |
| Replication timing policy (all constants) | `src/services/replication_service.cpp` |
| Node/subsystem wiring, protocol id | `src/net/p2p_transport.cpp` (~lines 230–330) |
| Torrent JSON codec | `src/domain/torrent_codec.cpp` — native `domain/torrent_codec.*` already ports this (M1); golden tests now prove it |
| Config defaults | `src/app/config_store.cpp` (~line 51: ports; ~54: p2p keys) |
| Search request parsing / limits | `src/services/search_service.h` Request + `peer_api.cpp` parseSearchRequest |

## Wire surface (names verbatim)

Requests ratsn must answer (query the local index via `SearchIndex`, reply to
the requesting peer only):
`searchTorrent`, `searchFiles`, `topTorrents`, `torrent`, `feed` (empty until
M6), `randomTorrents`.

Responses/pushes ratsn must consume (all writes funnel through
`Indexer::handleDiscovered` — classify → filter → upsert; never touch the
index directly):
`searchTorrent_response`, `searchFiles_response`, `torrent_response`,
`feed_response` (no-op until M6), `randomTorrents_response`, `torrentAnnounce`.

Wire quirks to preserve:
- Responses carry **no query echo and no correlation id**. The Qt UI appends
  whatever arrives to the currently displayed results. See "Remote search
  merge" below for how the TUI handles this.
- `torrentAnnounce` is **consume-only**: the Qt app registers a handler but
  never broadcasts it (grep confirms; only legacy JS-era peers send it). Do
  not invent a send side.
- `handleSearchRequest` ignores queries of length ≤ 2 — keep that guard.
- `randomTorrents` limit is clamped to [1,10] server-side; replies include
  file lists (`includeFiles=true`).

## New/changed native modules

1. **`native/engine/peer_api.{h,cpp}`** (~600 loc) — mirrors Qt PeerApi:
   `install()` registers every handler; request handlers answer from
   `SearchIndex`; response handlers funnel through the injected Indexer
   callback. Callbacks from librats MessageJson fire on worker threads —
   marshal onto EngineLoop before touching anything, same pattern as
   Crawler::onAnnounce. No exceptions across the librats boundary.
2. **`native/engine/peer_registry.{h,cpp}`** (~150 loc) — client_info
   handshake on connect + per-peer stats map (feeds the Status tab's future
   swarm totals; expose `connectedPeers()`/`remoteTorrentsCount()`).
3. **`native/engine/replication.{h,cpp}`** (~100 loc) — port of
   ReplicationService's adaptive interval verbatim (5 s initial, 10 s idle,
   60 s max, 3 s settle, busy threshold 8, backoff 600 ms/torrent, 5 per
   peer), driven by EngineLoop::postDelayed like Crawler's timers.
   `notifyReceived()` is called from peer_api's randomTorrents_response path.
4. **`NodeHost`** — attach the mesh: `MessageJson` (the on()/send() surface),
   `ReconnectionService` (store_path `<dataDir>/peers.json`),
   `PeerExchange` (peer_target from config). Wire the until-now dead
   `Config::upnp`/`Config::holePunch` fields to `PortMappingService`
   (enable_upnp+enable_natpmp) and `HolePunch` (enable_relay=true), matching
   p2p_transport.cpp. `listen_port` becomes `Config::p2pPort` (new, default
   4444), `max_peers` from new `Config::maxPeers` (Qt key `p2pConnections`,
   default 10). Keep protocol id `"rats-search/3"` exactly.
5. **`Config`** — new keys (names per Qt where the concept matches, §8 rule):
   `p2pPort` (4444), `maxPeers` (from `p2pConnections`, 10), `p2pReplication`
   (true), `p2pReplicationServer` (true; note Qt forces it true whenever
   p2pReplication is on, config_store.cpp ~148).
6. **TUI/console surfacing** — see below; `SearchHit.remote` and the
   `[peer]` badge already exist from M3.

## Remote search merge (TUI policy)

On a search trigger, after running the local query, broadcast `searchTorrent`
(and `searchFiles` when the files toggle is on) with the same request shape
the Qt UI sends (`src/ui/mainwindow.cpp` ~819). Arriving `*_response` hits are
marshalled to the UI thread and **appended to the current result list only if
the SearchTab generation hasn't moved** (reuse `generation_`): the wire gives
no correlation id, so the generation check is the native equivalent of the Qt
UI's "append to whatever is displayed" — same visible behavior when the query
is stable, no stale hits when the user has typed on. Dedup by hash against
hits already shown; stamp `remote=true`.

## Localhost acceptance setup (ports now collide by default!)

Since the 2026-08-24 commit, ratsn defaults `dhtPort=6881` — the same as the
Qt app. Running both on one machine for the M4 check therefore **requires**
overriding one side's ports in its data-dir config (e.g. ratsn test datadir:
`dhtPort: 16881`, `p2pPort: 14444`). For deterministic peer connection on
localhost (DHT-based mesh discovery is slow/flaky for a 2-node lab), add a
debug-only `--connect host:port` flag to `--console`/`--tui` that calls
`librats::Node::connect(host, port)` (node.h:139) after start. The Qt side
accepts inbound on its p2pPort (4444).

## Golden-file tests

- `native/tests/` with a tiny assert-based runner (no gtest — nothing new to
  port to the retro target), built behind `RATSN_BUILD_TESTS` (default OFF),
  run via ctest or directly.
- Fixture capture: behind an env var (e.g. `RATSN_WIRE_DUMP=1`), peer_api
  logs each received message's raw JSON to `<dataDir>/wire/<type>-<n>.json`.
  Run against the live Qt instance once, curate one fixture per message type
  into `native/tests/fixtures/`, commit them.
- Round-trip assertion per fixture: parse → `torrentFromJson` → `toJson` →
  compare as parsed JSON trees (field-by-field, order-insensitive), not as
  strings. A field the native codec drops or renames is a test failure — the
  §11 schema-drift guard.

## Session workflow constraints (unchanged from M2/M3)

- **No local builds without asking first** — the user builds/tests, including
  on the PPC target. Deliver via the established tarball flow:
  `git archive --prefix=rats-search-<short-sha>/ --format=tar.gz -o out.tar.gz HEAD`
  after each commit, send with SendUserFile.
- Keep the event-driven stdout diagnostics (M2 lesson); if any new component
  logs at DHT-operation frequency, route it through librats' Logger (file
  mode during TUI) — never a global stream redirect (M3 lesson, FTXUI renders
  via std::cout).
- Milestones stay strictly ordered: M4's §12 check must pass before M5.
