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

## Phase 1 -- the world (standalone, in progress)

- [x] Canonical wastes + stronghold graph stubs + Coil Yard graft
- [x] Starter inventory (`shiv`), wield/remove, `char.equipment`
- [x] Live bestiary + 2s heartbeat combat (`consider`/`attack`/`combat.*`)
- [x] Rest regen; `recall`/`home`
- [x] LocalHub-style traces: `listen`, `ping`, `ping all`, `inscribe`
- [x] Market `join`/`defend`; elf join brands ash-sworn (`valence: grave`)
- [x] `/map.svg` stub
- [x] Multiplayer: tell/reply/yell/emote, presence in `room.info`, `player.read`
- [x] Market `sell`/`steal` affordances; tavern talk/buy-dust labels
- [x] Rescue: holding pit (warden grace + antidote), cells, transit hub,
      `grid.rescued` / `grid.rescued_roll`, personal `char.dream` on sleep
- [ ] Economy: full shop/tavern vice resolution beyond affordances
- [ ] Full moral arc: forgiveness, redemption, reckoning, tide
- [ ] Racial abilities, title/`who`
- [ ] Persistence (SQLite; JSON store remains for now)
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
| Local Phase 1a (`127.0.0.1:8792`) | 71 | 79 | 2 | Combat, equipment, wastes map, join/ashsworn, listen/ping green |
| Local Phase 1b (`127.0.0.1:8792`) | 86 | 64 | 2 | Multiplayer tell/yell/emote/look + market economy affordances |
| Local Phase 1c (`127.0.0.1:8792`) | 95 | 58 | 1 | Rescue: holding pit grace, cells, transit hub, rescued roll, personal dream |

**Parity targets:** Rust Choir (Go) and Verdigris Spool (Python) prod baselines on the same upstream suite.
