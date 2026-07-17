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
- **Definition of done:** upstream `smoke.mjs` (**135 checks**)
- **Status:** scaffold only (Phase 0 not started). See `docs/PLAN.md`.
- **License:** AGPL-3.0-only (same as the other ports). See `LICENSE` + `NOTICE`.

## Quick start (scaffold)

```sh
make
./build/hollow-grid-c --help

# Later (when /ws is implemented):
# ./build/hollow-grid-c --addr 0.0.0.0:8792
# wscat -c ws://127.0.0.1:8792/ws
# MUD_URL=ws://127.0.0.1:8792/ws node /path/to/the-hollow-grid/smoke.mjs
```

## Dependencies (planned)

- C11 compiler (`clang` or `gcc`)
- SQLite3 (persistence)
- A WebSocket library (TBD in Phase 0: e.g. libwebsockets or similar)
- cJSON or equivalent for `@event` payloads

Exact pins land when Phase 0 chooses the stack; keep the surface minimal.

## Docs

| Doc | Purpose |
| --- | --- |
| [`docs/PLAN.md`](docs/PLAN.md) | Phase plan + smoke gate (mirror Go/Py) |
| [`docs/WORLD.md`](docs/WORLD.md) | Ferrite Wastes identity (not a clone of other worlds) |
| [`CLAUDE.md`](CLAUDE.md) | Contributor / agent working method |

## Skyphusion Labs

https://skyphusion.org · https://github.com/skyphusion-labs
