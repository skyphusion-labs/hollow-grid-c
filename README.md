# hollow-grid-c

A **Hollow Grid world server, in C.** The Hollow Grid is a federated MUD whose
reference implementation is TypeScript on Cloudflare Workers; this is a
from-scratch port of the *world half* as a distinct Grid node that speaks the
language-agnostic wire protocol and can join the federation when `GRID_HUB_URL`
is set.

Sibling ports (same contract, different runtimes):

| Port | World | Live (when deployed) |
| --- | --- | --- |
| [the-hollow-grid](https://github.com/skyphusion-labs/the-hollow-grid) (TS) | Hollow / Dustfall | hollow / dustfall `.skyphusion.org` |
| [hollow-grid-go](https://github.com/skyphusion-labs/hollow-grid-go) | Rust Choir | `wss://rustchoir.skyphusion.org/ws` |
| [hollow-grid-py](https://github.com/skyphusion-labs/hollow-grid-py) | Verdigris Spool | `wss://verdigris.skyphusion.org/ws` |
| **This repo (C)** | **Ferrite Wastes** (provisional; see `docs/WORLD.md`) | not live yet |

> The Hollow Grid is a dead network that outlived its makers. Worlds are nodes on
> that network; the shared backend *is* the Grid. Ferrite Wastes is where magnetic
> memory still holds a charge the network never collected: choices made legible as
> data, not buried in prose.

- **Upstream contract:** [`the-hollow-grid/docs/protocol.md`](https://github.com/skyphusion-labs/the-hollow-grid/blob/main/docs/protocol.md)
- **Definition of done:** upstream `smoke.mjs` (**153 executable standalone
  checks** on the 2026-07-17 revision)
- **Status:** Phase 0 transport foundation in progress. `/ws`, login/resume,
  structured events, health probes, and the Coil Yard opening map are playable.
  See `docs/PLAN.md`.
- **License:** AGPL-3.0-only (same as the other ports). See `LICENSE` + `NOTICE`.

## Quick start

```sh
# macOS
brew install libwebsockets cjson pkgconf

# Debian / Ubuntu
# sudo apt-get install libcjson-dev libwebsockets-dev pkg-config

make
./build/hollow-grid-c --addr 127.0.0.1:8792
wscat -c ws://127.0.0.1:8792/ws

# From a sibling the-hollow-grid checkout:
MUD_URL=ws://127.0.0.1:8792/ws node /path/to/the-hollow-grid/smoke.mjs
```

`make check` builds with strict warnings and runs the core world, event, and
name-store tests.

## Dependencies

- C11 compiler (`clang` or `gcc`)
- libwebsockets (HTTP and WebSocket transport)
- cJSON (`@event` payloads and the Phase 0 character store)
- pkg-config (build flag discovery)

Phase 0 persists name-keyed characters as JSON under `DATA_DIR/characters`.
SQLite replaces this small store when the full Phase 1 world state lands.

## Docs

| Doc | Purpose |
| --- | --- |
| [`docs/PLAN.md`](docs/PLAN.md) | Phase plan + smoke gate (mirror Go/Py) |
| [`docs/WORLD.md`](docs/WORLD.md) | Ferrite Wastes identity (not a clone of other worlds) |
| [`CLAUDE.md`](CLAUDE.md) | Contributor / agent working method |

## Skyphusion Labs

https://skyphusion.org · https://github.com/skyphusion-labs
