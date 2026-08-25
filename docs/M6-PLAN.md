# M6 implementation plan — downloads tab + session resume

Written 2026-08-25 for the session implementing M6 (runs AFTER the M5
session; see docs/M5-PLAN.md). Read alongside docs/DESIGN-native.md (§10
milestone row, §12 acceptance). This is the milestone that turns ratsn into a
complete app: search → find → download → resume across restarts.

The governing rule applies in full here — **the Qt code is the behavioral
spec: port it, don't improve it.** `src/services/download_service.cpp` is a
clean, recently-refactored service with documented threading and restore
semantics; the port is mostly a Qt→native mechanical translation plus a TUI
front-end. Named deviations are listed under "Deliberate deviations".

## Goal and acceptance

A torrent found in the Search/Top tab can be downloaded to completion from
inside the TUI, and an unfinished download resumes after the app restarts
(DESIGN-native.md §12 M6). Console mode resumes and completes session
downloads headless.

## Scope decisions

- **No per-file selection UI.** librats downloads all files; Qt's
  `DownloadService::selectFiles` only records the selection locally "until it
  [librats] gains per-file selection support" (download_service.cpp ~404).
  Port the `selected` field through the session schema for file-format
  compatibility, but build no UI for a knob that does nothing.
- **No torrent creation / drag-drop / `registerSeed`.** Qt's TorrentCreator
  path (hash a local folder, seed it, add to index) is a separate feature —
  out of scope, not renumbered anywhere yet; revisit after M8 if wanted.
- **`removeOnDone`**: keep the field in the Download struct + session JSON
  (schema parity), no TUI control for it.
- The 1s progress poll, speed derivation, restore-seeds-live-state trick, and
  "no async failure signal" policy (metadata that never arrives just keeps
  retrying — only synchronous add errors are reported) are all ported as-is.

## Spec sources (read before writing code)

| What | Where |
|---|---|
| The whole service: add/restore/pause/remove lifecycle, 1s poll, transitions, speed calc, restore-vs-recheck comment | `src/services/download_service.cpp` (+ .h struct fields) |
| Session file shape (`torrents_session.json`) — copy field names verbatim from the `toJson()` helpers | `src/services/torrent_session_store.cpp`, `Download::toJson`/`DownloadFile::toJson` in download_service.cpp |
| Session load/save call sites + shutdown ordering | `src/app/application.cpp` ~252 (load after transport start), ~276 (save in stop(), before transport stop) |
| librats download API (all of it exists: `add_magnet`, `add_magnet_resumed`, `add_torrent_file`, `pause_torrent`/`resume_torrent`, `remove_torrent(hash, delete_files)`, `save_resume_data`, `save_all_resume_data`, thread-safe `torrent_status`) | `src/librats/src/librats/bittorrent/client.h` (TorrentStatus struct ~line 43) |
| How Qt translates Client calls / snapshots, magnet URI prefix, info-hash parsing | `src/net/torrent_engine.cpp` (`addMagnet` ~162, `status` ~292, `parseInfoHash`) |
| Client access from the subsystem | `librats::Bittorrent::client()` — native NodeHost already exposes `bittorrent()` (node_host.h:59) |
| Download-path config key | `src/app/config_store.cpp` ~68 (`downloadPath`, default = OS download dir) |
| TUI 1s-poll → UI-thread snapshot idiom to copy | `native/tui/status_bar.h` `StatusUpdater` |

## Work items

### 1. Decouple NodeHost from the spider flag (`native/main.cpp`)

`SpiderPipeline` currently skips EVERYTHING when `cfg.spider` is off
(main.cpp ~157) — but downloads need the node + Bittorrent client regardless.
Restructure to match Qt (application.cpp: transport always starts, crawler
only if `indexerEnabled()`): NodeHost + mesh (PeerApi/Replication/registry)
start whenever `--console`/`--tui` runs; only the Crawler stays gated on
`cfg.spider`. Rename the struct (it is no longer just the spider pipeline).
This is a structural change to validated M4 code — keep it a mechanical
regrouping, no behavior change beyond the gating.

### 2. `native/engine/downloads.{h,cpp}` — DownloadManager (~450 loc)

Port of Qt `DownloadService` + the thin slice of `TorrentEngine` it uses
(add/pause/resume/remove/status translation, `parseInfoHash`), with one
architectural translation: **single-threaded on the EngineLoop, no mutex.**
Qt needs a QMutex because UI/REST threads call in directly; in ratsn every
mutation is posted to the EngineLoop (the established pattern), and librats'
`torrent_status()` snapshots are documented thread-safe. Keep the poll
structure (`pollStatus` → `Transitions` → flush) — it reads well and the
restore/recheck subtleties live there.

- State: `std::map<std::string, Download>` (native structs mirroring
  Qt's `Download`/`DownloadFile`, `librats::Json toJson()` mirroring the Qt
  field names exactly — the session file must stay Qt-shaped).
- Poll: 1s self-rescheduling `EngineLoop::postDelayed` (Crawler/StatusUpdater
  idiom). Port the transition logic verbatim: metadata-arrival →
  `applySnapshot`, completion edge (+`removeOnDone` handling), speed from the
  cumulative byte counter, skip-unchanged-snapshot suppression (keep it — the
  TUI reads snapshots, and idle churn costs redraws).
- Add paths: `add(magnetOrHash, savePath)`, `addWithInfo(const
  domain::Torrent&, savePath)` (used by the TUI's `d` key — name/size show
  immediately), `addFromFile(path, savePath)`. All three: validate/normalize
  the hash, dedup, `ensureDir`, then the corresponding
  `client->add_magnet/add_torrent_file` call with the exact
  `"magnet:?xt=urn:btih:" + hash` URI shape torrent_engine.cpp uses. Do NOT
  retain the returned `Torrent*` (owned by the client; Qt only null-checks
  it).
- Restore: port `restore()` including its two subtleties (seed live state
  from the session so a finished torrent doesn't flash 0% during librats'
  async recheck; never let a pre-recheck snapshot downgrade
  `completed=true`), and `loadSession`'s pause/selection reapplication.
- Guard every client call on `nodeHost->bittorrent()->client() != nullptr`
  (Qt `isReady()` — the subsystem can be down).
- Events: no signal system. The TUI polls snapshots (item 5); expose
  `std::vector<Download> snapshot() const` (engine-thread only) plus an
  optional completion callback for the status-bar flash, marshalled the same
  way Crawler's callbacks are.
- Diagnostics through `platform::log()` only (M4 lesson), matching the Qt
  service's log lines (add/restore/complete/remove).

### 3. `native/engine/session_store.{h,cpp}` (~120 loc)

Port of `TorrentSessionStore`: pure JSON file I/O for
`<dataDir>/torrents_session.json` — same filename, same field names.
Serialize through the same `toJson()` helpers the live path uses (the
store's own header comment explains why — keep that property). Empty list
removes the file. Malformed/missing file → empty vector.

### 4. Session lifecycle + config (`main.cpp`, `platform/config.*`)

- Load: after the pipeline starts (both `--console` and `--tui`), mirror
  application.cpp:252.
- Save: in the shutdown sequence of BOTH commands, **before
  `nodeHost->stop()`** (Qt saves before transport stop): per-torrent
  `save_resume_data` then the JSON write, exactly `saveSession()`'s order.
- Config: new key `downloadPath` (Qt's name). Default: `$HOME/Downloads`
  when `HOME` is set, else `<dataDir>/downloads` (Qt uses the OS download
  location; this is the closest portable equivalent — document it).

### 5. `native/tui/downloads_tab.{h,cpp}` (~350 loc) + keys

- Third content tab (Search / Top / Downloads / Status — Tab-cycling is
  already modulo after M5). List rows: name, progress bar or %, size
  (downloaded/total via M3's `humanSize`), speed, peers, state
  (`fetching metadata` / `downloading` / `paused` / `seeding` /
  `completed`). Details pane: save path + file list (no per-file progress —
  librats doesn't report it; Qt fakes 1.0 on completion only).
- Refresh: a 1s updater exactly like `StatusUpdater` — engine-thread
  `snapshot()`, post the vector to the UI thread. No per-event plumbing.
- Keys (all routed through the M5 `anyInputFocused()` guard):
  - `d` on a Search/Top result (lives in the shared result view from M5):
    `addWithInfo` with the full record to the default `downloadPath`. Show
    "already downloading" feedback when dedup rejects.
  - Downloads tab: `space` toggle pause, `x` remove (keeps files on disk,
    saves resume data — so `x` is always recoverable), `X` pressed twice on
    the same row within a few seconds = remove AND delete files
    (`remove_torrent(hash, delete_files=true)`).
  - An add-input row: paste a magnet URI, bare 40-hex hash, or absolute path
    to a `.torrent` file (dispatch on the shape; covers all three Qt add
    paths — including .torrent files saved by M5's `t` key).
- Status bar: append active-download count + aggregate speed to the existing
  1Hz `StatusModel` (e.g. `dl:2 1.2M/s`), zero-cost from the same snapshot.

### 6. Console mode

Nothing downloads-specific beyond items 1+4: with the session loaded and the
poll running on the EngineLoop, `--console` resumes and completes unfinished
downloads headless (progress visible via the existing stats line — extend it
with the same `dl:` summary). No add path in console (REST stays deferred,
§11); adds happen in the TUI, resumption works anywhere.

## Deliberate deviations from Qt (all marked in comments)

1. **No mutex / signal layer** — EngineLoop ownership replaces both (§ item 2
   rationale).
2. **60s autosave**: every 60s, `client->save_all_resume_data()` + session
   JSON write. Qt persists only on clean shutdown, which loses the session on
   power loss/crash — cheap insurance that matters on period hardware. Skip
   the write when the snapshot is unchanged since the last save.
3. **`X`-deletes-files**: librats supports `delete_files=true`; Qt's UI never
   exposes it. Double-press confirmation, log the deletion.
4. Default download path fallback (`<dataDir>/downloads`) when `$HOME` is
   unset.

## Acceptance (DESIGN-native.md §12 M6, expanded)

- `d` on a search result starts a download; it reaches 100% and the file
  opens/checksums correctly (owner's end; pick a well-seeded torrent).
- Pasting a magnet URI and a `.torrent` path into the add row both start
  downloads.
- Kill the app mid-download (both `q` and SIGTERM), restart: the download
  reappears at its prior progress and completes. A completed torrent
  reappears as completed/seeding immediately (no 0% flash) — the restore
  subtlety working.
- A paused download is still paused after restart.
- `--console` with a session file resumes and completes a download headless.
- `x` then re-add resumes from the kept pieces; `X X` leaves nothing on disk.
- TUI stays responsive during an active download (poll is 1s snapshots, no
  per-piece events).

## Session workflow constraints (unchanged from M4/M5)

- **No local builds without asking first** — the owner builds/tests
  (including PPC). Tarball flow after each commit:
  `git archive --prefix=rats-search-<short-sha>/ --format=tar.gz -o out.tar.gz HEAD`,
  send with SendUserFile.
- **All engine/TUI diagnostics through `platform::log()`** — never bare
  `std::cout`/`std::cerr` (the TUI redirects the sink to
  `<dataDir>/ratsn-engine.log`; FTXUI renders via std::cout).
- Item 1 touches validated M4 wiring — re-run a quick mesh sanity check
  (peers connect, replication inserts) as part of acceptance, not just the
  download checks.
- Milestones stay strictly ordered: M6 starts only after M5's acceptance
  passes on the owner's build; M7 (votes/feed) waits for M6's.
