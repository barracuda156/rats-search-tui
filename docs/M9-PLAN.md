# M9 implementation plan — optional borealis/nanovg GUI

Written 2026-08-27 (owner's call: the plan-doc deferral in DESIGN-native.md
§10 is lifted; the two outstanding gates — the SDL text-input probe and the
borealis fork/tag pin — become Stage 0 items below instead of blockers on
writing this plan). Read alongside docs/DESIGN-native.md (§10 milestone row
+ toolkit-decision note, §12 acceptance).

M9 adds a second, optional front-end shell beside the TUI: borealis
(xfangfang's fork) + nanovg + yoga, over SDL2 on the retro targets or GLFW
on newer systems. **The toolkit decision is settled and not to be
relitigated** (DESIGN-native.md §10): the owner already runs this exact
stack on PPC/BE 10.6 — xfangfang/wiliwili#570, including the big-endian
color fixes — so the stack's portability is a solved problem, not a risk
item. Qt4 and Cocoa were considered and rejected; upstream Qt6 `src/ui/`
(~6.5k lines) is welded to Qt6-only classes and is a **UX blueprint only**
— every line of GUI code here is new, over the existing engine.

Unlike M1–M8 there is no Qt code to port line-by-line: the governing rule
becomes **the TUI is the behavioral spec for engine interaction** (thread
confinement, generation guards, revision polling, debounce constants), and
`src/ui/` is the spec for *what the user sees* (layout, columns, details
fields). Where the two disagree on a flow, follow the TUI — it is the
already-accepted translation of Qt behavior onto this engine.

## Scope decisions (owner-ratified defaults; flag deviations in review)

- **In scope**: Search / Top / Feed / Downloads / Status views, votes,
  download-to-completion, poster fetch + a poster-grid browse view (§12
  acceptance line). Keyboard-first with full mouse support.
- **Out of scope**: settings dialog (config stays file-based, as the TUI),
  favorites/activity widgets (no native engine service behind them),
  torrent creation, REST. Do not port `src/ui/settingsdialog.*`,
  `favoriteswidget.*`, `activitywidget.*`.
- **Code-built views, no app-side XML layouts.** borealis supports XML
  inflation (wiliwili uses it heavily) but that adds an asset-shipping
  surface for zero benefit at ratsn's view count. Only borealis's own
  `resources/` (fonts, themes, icons) ships.
- **`RATSN_WITH_GUI` default OFF.** The TUI build must be byte-identical
  with the option off — `native/engine|index|platform` stay
  front-end-free (no borealis includes outside `native/gui/`).
- **borealis is a vendored, owner-pinned submodule** (the `src/librats`
  pattern, not FTXUI's find_package pattern — borealis has no packaged
  releases and the fork/commit matters).

## Stage 0 — prerequisites (owner + one short session; no GUI code)

0a. **SDL text-input probe on the retro target.** Commit
    `native/tools/sdl_textinput_probe.c`: a single-file SDL2 program (built
    standalone: `cc $(sdl2-config --cflags --libs)`) that opens a window,
    calls `SDL_StartTextInput()`, and logs every `SDL_TEXTINPUT` /
    `SDL_TEXTEDITING` / `SDL_KEYDOWN` event plus `SDL_GetClipboardText()`
    on Ctrl/Cmd+V. Owner runs it on 10.6/PPC under (1) a Latin layout,
    (2) a Cyrillic layout, (3) a CJK IME, (4) clipboard paste. Record the
    results in this file. Accepted fallback per DESIGN-native.md: paste
    covers typed CJK; layouts (1)(2) arriving via SDL_TEXTINPUT is the
    hard requirement for the search box.
0b. **Borealis pin (owner, 2026-08-27): `github.com/xfangfang/borealis`,
    branch `wiliwili` (the fork's default), commit
    `5f08b286f3df737f3321d2247a6fe633fcead03c`** — this is both what
    wiliwili's own submodule points at and the current branch tip (verified
    identical via the GitHub compare API at pin time; tip commit dated
    2026-04-25, "Fix the crash caused by incorrect key bindings"). Stage A
    adds the submodule at exactly that SHA, `branch = wiliwili` in
    .gitmodules for future bumps. Still owner-supplied before Stage A:
    which backend the PPC port validated (presumably SDL2) and whether that
    port carries local overlay patches on top of this borealis commit —
    if it does, those patches are part of the pin.
0c. **M7 acceptance passes** (two-node votes/feed check, docs/M7-PLAN.md)
    — the sequencing rule in DESIGN-native.md §12 holds; M7 is currently
    unbuilt on the owner's side (needs librats `-DRATS_STORAGE=ON`). The
    GUI renders M7's features, so implementation (Stage B onward) waits.

## Spec sources (read before writing code)

| What | Where |
|---|---|
| Engine surface the GUI consumes: the exact `tui::run(...)` parameter list (EngineLoop, engineThread, index, nodeHost, crawler, peerApi, downloads, trackerService, voting, feed, cfg, StatusInfo) and its contract (joins engineThread before returning) | `native/tui/app.{h,cpp}`, `native/main.cpp` `cmdTui` |
| Per-view engine interaction: debounced search-as-you-type, generation guards, `engineLoop_.post` → `screen_.Post` round-trips, Top/Feed activation reload + `Feed::revision()` polling, downloads 1s poll, selection-triggered tracker refresh, vote flow | `native/tui/search_tab.cpp`, `top_tab.cpp`, `feed_tab.cpp`, `downloads_tab.cpp`, `result_view.cpp` |
| UX blueprint (layout/columns/fields only, no code reuse): main window + tabs, results table columns, details panel field order, feed/downloads presentation | `src/ui/mainwindow.cpp`, `torrenttablewidget.cpp`, `torrentdetailspanel.cpp`, `feedwidget.cpp`, `downloadswidget.cpp`, `toptorrentswidget.cpp` |
| Stack integration reference: borealis init on desktop, SDL2-vs-GLFW backend selection, main-loop shape, resource path handling, list widgets at scale (RecyclingGrid) | owner's pinned wiliwili/borealis trees (Stage 0b) |
| Poster source + fetch machinery | `info["poster"]` written by M8's site scraper (`native/engine/site_scraper.cpp`), `native/platform/worker_pool.{h,cpp}` |

**Verify the borealis API surface against the pinned fork's actual headers
before writing against it** (the M8 rule, doubly so here: forks diverge —
do not trust upstream borealis docs for `brls::sync`, input, or widget
names).

## Stages (one Sonnet session each, tarball + owner check between)

### Stage A — skeleton (~0.5k loc): boots, quits, Status view

- CMake: `RATSN_WITH_GUI` option (default OFF); vendored borealis
  submodule at the Stage 0b pin; `RATSN_GUI_BACKEND` (SDL2 default,
  GLFW alternative) forwarded to the fork's own backend flags, mirroring
  wiliwili's desktop invocation. Resource shipping: install/copy the
  borealis `resources/` tree and resolve it at runtime relative to the
  binary (verify the fork's mechanism — compile define vs runtime path).
- `native/gui/app.{h,cpp}`: `gui::run(...)` with the same signature and
  contract as `tui::run` (joins engineThread before returning).
  `native/main.cpp`: `--gui` verb gated by `#ifdef RATSN_WITH_GUI`,
  `cmdGui` cloned from `cmdTui` (same pipeline construction, load order,
  shutdown order); printUsage updated.
- `native/gui/marshal.{h,cpp}`: the engine→UI post helper (expected:
  `brls::sync`; verify) — the GUI-side analog of `screen_.Post`, used by
  every later stage. UI→engine stays `engineLoop_.post`, unchanged.
- Tab frame with a Status view (port of the TUI status tab's data flow:
  the periodic status snapshot posted from the engine thread).
- Font check: confirm the shipped/loaded font covers Latin + Cyrillic;
  identify the CJK fallback font path on target (10.6 ships Hiragino/
  STHeiti) and wire borealis font fallback if the fork supports it —
  needed by Stage B's CJK-by-paste acceptance.
- Gate: owner builds on x86 first, then PPC; window opens over the live
  engine, status numbers tick, `q`/close quits cleanly, TUI build
  unaffected with the option off.

### Stage B — search (~0.8k loc): the make-or-break stage

- `native/gui/result_list.{h,cpp}`: shared results-list + details-pane
  pair (the GUI's `ResultView`): columns per `torrenttablewidget.cpp`,
  details fields per `torrentdetailspanel.cpp` (name, hash, size/files,
  seeders + checked-age, added, type/category, votes, description,
  trackers, matching files). Selection triggers the tracker refresh
  exactly like `ResultView::onSelectionChanged`; in-place row refresh via
  the `TrackerService` callbacks (register alongside the TUI's, through
  the marshal helper). List widget: whatever the pinned fork offers that
  recycles cells; wiliwili's RecyclingGrid is the reference if stock
  borealis lists don't scale to 100-row result sets.
- `native/gui/search_view.{h,cpp}`: search box (SDL text input path per
  Stage 0a results; paste must work) + strict/safe toggles + the TUI's
  debounce constants and generation guard, remote results appended via
  PeerApi callbacks as in `search_tab.cpp`.
- Actions on the selected row: download (`d`/button), magnet copy to
  clipboard (the GUI upgrade of the TUI's magnet status-line), save
  .torrent, vote good/bad (`v`/`V` + details-pane buttons) with the
  result message surfaced (toast/status strip).
- Gate: §12's search flows — search-as-you-type incl. Cyrillic typed and
  CJK pasted, download starts, votes register (M7 storage build).

### Stage C — remaining views (~0.7k loc)

- `top_view` (content-type + time toggles, reload on activation),
  `feed_view` (activation reload + `Feed::revision()` poll — the GUI
  main loop needs a per-frame or timer hook for it; verify what the fork
  offers), `downloads_view` (1s poll, progress bars, pause/resume/delete
  with the TUI's delete-files confirm flow).
- Gate: every remaining TUI flow works in the GUI.

### Stage D — posters (~0.5k loc): where this stack earns its keep

- `native/gui/poster_cache.{h,cpp}`: fetch `info["poster"]` URLs via
  libcurl on a `WorkerPool` (M8's httpGet idiom: redirects, timeout,
  cancellation, UA), cap 3 concurrent, only for on/near-screen cells;
  disk cache at `<dataDir>/posters/<hash>` (extension from content;
  nanovg's stb_image decodes png/jpg/gif/bmp); size cap per image (skip
  multi-MB posters) and a total-cache cap with LRU eviction. Decode via
  `nvgCreateImageMem` on the UI thread (GL context affinity), bytes read
  on the pool.
- Poster in the details pane (when present) + a poster-grid browse view
  over Top/Feed data.
- Gate: §12 M9 acceptance in full, on the PPC target.

## Deliberate deviations from the Qt6 UI (all marked in comments)

1. **No settings dialog / favorites / activity** — out of scope (no
   engine service behind them; config is file-based).
2. **Code-built views instead of XML layouts** — smaller asset surface.
3. **Poster disk cache** — Qt6 kept posters in an in-memory pixmap cache
   only; the retro target wants re-fetch avoidance across runs.
4. **Toast/status strip instead of modal message boxes** for action
   results (matches the TUI's status-line idiom).

## Acceptance (DESIGN-native.md §12 M9)

With `RATSN_WITH_GUI=ON`: every TUI flow works in the GUI
(search-as-you-type including CJK-by-paste, download to completion, feed
browse, vote); M8 posters render in a browse view; runs on the PPC target;
the TUI build is unaffected when the option is off. Stage gates above are
the incremental checkpoints toward this.

## Session workflow constraints (unchanged, plus GUI-specific)

- **No local builds without asking first** — owner builds/tests (the GUI
  additionally needs SDL2/GL on the build box; assume owner-side only).
- **All diagnostics through `platform::log()`** — GUI frame loops are the
  worst place for stdout chatter; nothing may print to the terminal the
  app was launched from except startup errors.
- Verify borealis/nanovg/SDL API surfaces against the pinned fork's real
  headers before writing against them.
- Keep `native/engine|index|platform` free of any GUI include; the GUI
  registers its callbacks beside the TUI's patterns, never instead of
  refactoring engine code.
- One stage per session; do not start the next stage before the owner
  confirms the previous stage's gate.
