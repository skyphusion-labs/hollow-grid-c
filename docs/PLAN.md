# Build plan and status

Porting the Hollow Grid world framework to C, against upstream
`the-hollow-grid/docs/protocol.md`. The scoreboard is upstream `smoke.mjs`
(**159 executable checks** on the 2026-07-17 revision): build the
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
- [x] Economy: tinker `list`/`buy`, tavern `buy dust`/`use dust`, market `sell`
- [x] Racial abilities (Human Requisition + cooldown), `title`, `who`/`grid.who`
- [x] Full moral arc: stray/return, forgive, dais defy, reckoning, local tide/`war`
- [x] Support surface: wall, cache/gather, give, treat, witness, listen echoes,
      local gridstats/gridprune
- [x] LocalHub federation surface: gridcast relay, whoami, worlds, travel
- [x] Persistence: SQLite character store (`DATA_DIR/characters.db`), one-time
      import of legacy per-character JSON files
- [x] Standalone smoke green (153 ok / 0 fail / 1 skip; skip = second world)

## Phase 2 -- federation

- [x] LocalHub federation fallback (gridcast poll, seeded worlds/Saltreach, whoami)
- [x] Remote Grid Hub HTTP/JSON client (`GRID_HUB_URL` via libcurl)
- [x] Federation loop over RemoteHub (register, presence, cast relay, tide cache)
- [x] Background federation worker (periodic RPC + identity commits stay off the
      WebSocket event loop)
- [x] Command-path hub RPC also off the loop: cached worlds/presence/tide/ledger/
      hub health; queued fallen/gridcast/tide-shift/prune; async CharSheet load
      on login (#14, #15)
- [x] Canonical CharSheet merge on login / commit on disconnect+travel
- [x] Live hub smoke with Dustfall; two consecutive stateful runs on one process
      pass all 159 checks

## Phase 3 -- container + release

- [x] `Dockerfile` + `compose.yaml` + `.env.example`
- [x] GHCR release workflow (`.github/workflows/release.yml`)
- [x] Fleet stack + public `wss://ferrite.skyphusion.org/ws` (fleet-chezmoi)
- [x] mud-bots soak: Slag + Scale travelling (partial unpark 2026-07-18)
- [x] Blocking CI smoke gate + hub response cap (#16, #17 via #19)
- [x] Coverage ratchet: unit seams for store/grid clients, `gcovr --fail-under-line`
      floor, and instrumented-smoke headroom (900s / 30-min job)
- [x] SQLite persistence (Phase 1 carryover; #18)

## Smoke baselines

| Target | ok | fail | skip | Notes |
| --- | ---: | ---: | ---: | --- |
| Local Phase 0 (`127.0.0.1:8792`) | 36 | 115 | 2 | Transport gate green; failures are Phase 1 game, LocalHub, map, and federation features |
| Local Phase 1a (`127.0.0.1:8792`) | 71 | 79 | 2 | Combat, equipment, wastes map, join/ashsworn, listen/ping green |
| Local Phase 1b (`127.0.0.1:8792`) | 86 | 64 | 2 | Multiplayer tell/yell/emote/look + market economy affordances |
| Local Phase 1c (`127.0.0.1:8792`) | 95 | 58 | 1 | Rescue: holding pit grace, cells, transit hub, rescued roll, personal dream |
| Local Phase 1d (`127.0.0.1:8792`) | 108 | 45 | 1 | Economy + identity: shop, buy dust, ability, title, who |
| Local Phase 1e (`127.0.0.1:8792`) | 126 | 27 | 1 | Moral arc: stray/return, forgive, defy, reckoning, local tide |
| Local Phase 1f (`127.0.0.1:8792`) | 146 | 7 | 1 | Support: wall, cache/gather, give, treat, witness, listen echo, ledger |
| Local Phase 2a (`127.0.0.1:8792`) | 153 | 0 | 1 | LocalHub federation: gridcast, whoami, worlds/travel; skip = Dustfall |
| Local Phase 2b (`127.0.0.1:8792`) | 153 | 0 | 1 | RemoteHub client wired; standalone still LocalHub when unset |

**Parity targets:** Rust Choir (Go) and Verdigris Spool (Python) prod baselines on the same upstream suite.

Standalone scoreboard matches Verdigris Spool's local baseline. Set `GRID_HUB_URL`
(+ `GRID_HUB_TOKEN`) to join the live hub; Phase 12 Dustfall smoke still needs a
reachable second world.
