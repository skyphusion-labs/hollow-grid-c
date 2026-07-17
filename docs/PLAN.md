# Build plan and status

Porting the Hollow Grid world framework to C, against upstream
`the-hollow-grid/docs/protocol.md`. The scoreboard is upstream `smoke.mjs` (**135
checks**): build the port to pass it, phase by phase.

Sibling references: `hollow-grid-go/docs/PLAN.md`, `hollow-grid-py/docs/PLAN.md`.

## Phase 0 -- transport foundation (not started)

- [ ] Choose WebSocket + JSON stack (document in README); keep deps minimal
- [ ] Project build (`Makefile` / CI compile)
- [ ] **Ferrite Wastes** world identity (`docs/WORLD.md`) -- refine voice/map as content lands
- [ ] Canonical anchor map seed (federation-required ids) + local graft rooms
- [ ] `/ws` WebSocket, UTF-8 text, CRLF lines
- [ ] Login flow: banner, name, race menu, play; name-based identity
- [ ] `@event` channel framing
- [ ] `/health` + `/health/deep`
- [ ] Transport smoke against upstream `smoke.mjs` (login, resume, move subset)

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
| (none yet) | -- | -- | -- | Scaffold only |

**Parity targets:** Rust Choir (Go) and Verdigris Spool (Python) prod baselines on the same upstream suite.
