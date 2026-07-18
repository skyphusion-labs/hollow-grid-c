# Build plan and status

Porting the Hollow Grid world framework to C, against upstream
`the-hollow-grid/docs/protocol.md`. The scoreboard is upstream `smoke.mjs`
(**153 executable standalone checks** on the 2026-07-17 revision): build the
port to pass it, phase by phase.

Sibling references: `hollow-grid-go/docs/PLAN.md`, `hollow-grid-py/docs/PLAN.md`.

## Phase 0 -- transport foundation (done)

- [x] Minimal stack: libwebsockets + cJSON (documented in README)
- [x] Strict C11 project build and core tests (`make check`) + CI dependencies
- [x] **Ferrite Wastes** identity: hysteresis node + Coil Yard (`docs/WORLD.md`)
- [x] Canonical opening map + Coil Yard graft west from `roof`
- [x] `/ws` WebSocket, UTF-8 text, CRLF lines
- [x] Login flow: banner, name, structured race menu, play; name-based identity
- [x] Phase 0 JSON character store; persistent resume skips race creation
- [x] `@event` framing: creation, room, vitals, affects, actions, world state
- [x] `/health` + `/health/deep` with non-critical local hub check
- [x] Upstream smoke transport gate: health, login, resume, move, and rat room green

## Phase 1 -- the world (standalone)

- [ ] Full canonical + endgame map, bestiary, items, equipment
- [ ] Heartbeat tick: combat, regen, `world.state`
- [ ] Moral arc: Cinder Front, ash-sworn, redemption, reckoning
- [ ] Economy: shop, tavern vices, steal/sell
- [ ] Multiplayer: tell/reply/yell/emote, presence
- [ ] Rescue: holding pit, cells, transit hub, `grid.rescued`
- [ ] Grid commands: ping, listen, war, gridcast, witness, cache/gather
- [ ] Persistence (SQLite)
- [ ] `LocalHub` federation fallback + `/map.svg`
- [ ] Standalone smoke green (or documented residual fails)

## Phase 2 -- federation

- [ ] Remote Grid Hub HTTP/JSON client (`GRID_HUB_URL`)
- [ ] Federation loop (register, presence, gridcast relay, tide cache)
- [ ] Canonical CharSheet merge on login / commit on persist
- [ ] Live hub smoke with a second world (`DUSTFALL_URL`)

## Phase 3 -- container + release

- [ ] `Dockerfile` + `compose.yaml`
- [ ] GHCR release workflow
- [ ] Fleet stack + public `wss://…` hostname (fleet-chezmoi)
- [ ] mud-bots soak (optional; after transport+combat stable)

## Smoke baselines

| Target | ok | fail | skip | Notes |
| --- | ---: | ---: | ---: | --- |
| Local Phase 0 (`127.0.0.1:8792`) | 36 | 115 | 2 | Transport gate green; failures are Phase 1 game, LocalHub, map, and federation features |

**Parity targets:** Rust Choir (Go) and Verdigris Spool (Python) prod baselines on the same upstream suite.
