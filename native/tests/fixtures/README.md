# Wire fixtures

Golden files for `wire_codec_test` (docs/M4-PLAN.md's "Golden-file tests"
section). Each `*.json` here is one message captured from a **live Qt
rats-search instance**, curated by hand -- not synthesized, since the point
is verifying the native codec against real wire output, not against our own
assumptions about it.

## Capturing

1. Run a Qt `rats-search` instance and let it collect a few torrents.
2. Run `ratsn --console` (or `--tui`) against the same peer mesh with
   `RATSN_WIRE_DUMP=1` set, and let it connect (see `--connect HOST:PORT` in
   `ratsn -h` for a deterministic localhost pairing -- DHT-based discovery is
   slow for a two-node lab).
3. Every message `ratsn` receives is logged to `<dataDir>/wire/<type>-<n>.json`.
4. Pick one representative fixture per message type ratsn's peer_api.cpp
   handles as a *request* payload it also emits as a *response* (the round
   trip this test checks is `torrentFromJson -> toJson`, so only fixtures
   that are themselves a torrent-shaped payload -- `searchTorrent_response`,
   `torrent_response`, one entry from `randomTorrents_response`'s
   `"torrents"` array, `torrentAnnounce` -- are meaningful here).
5. Copy the chosen file(s) into this directory, named descriptively (e.g.
   `torrent_response-with-files.json`), and commit them.

## What the test checks

`wire_codec_test` parses each fixture, runs it through
`torrentFromJson(json) -> toJson(torrent)`, and compares the result against
the original fixture field-by-field (order-insensitive for objects). A field
the native codec drops or renames shows up as a mismatch -- that is the
schema-drift guard docs/DESIGN-native.md §11 calls for.

Until fixtures exist here, the test passes trivially (nothing to check) --
that's expected pre-capture, not a bug.
