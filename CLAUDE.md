# CLAUDE.md

> ## HANDS-OFF -- Conrad's solo fun
>
> **No agent work in this repo without explicit task language from Conrad in the same
> turn.** Prior foreign-agent damage on the Basalt (asm) lane is why this rule is hard.
> Stop means stop. If you are not under an explicit task for this repo right now, leave.
> Protocol authority remains `the-hollow-grid/docs/protocol.md`.

Guidance for Claude Code / agents working in this repo (only when explicitly tasked).

## What this is

**Ferrite Wastes: a Hollow Grid world server, in C.** Sibling of
[hollow-grid-go](https://github.com/skyphusion-labs/hollow-grid-go) (Rust Choir) and
[hollow-grid-py](https://github.com/skyphusion-labs/hollow-grid-py) (Verdigris Spool).
Reference implementation:
[the-hollow-grid](https://github.com/skyphusion-labs/the-hollow-grid) (TypeScript on
Cloudflare Workers + Durable Objects).

This is a **complementary fleet world node**, not a Workers/DO port. It speaks the
same language-agnostic wire protocol and joins the Grid Hub when `GRID_HUB_URL` is set.

**Protocol authority:** `the-hollow-grid/docs/protocol.md`. **Definition of done =
upstream `smoke.mjs`** (score against current the-hollow-grid head); two consecutive
stateful RemoteHub runs pass.

**Status:** **Ferrite live** on the Grid when healthy. See `docs/PLAN.md` and
`docs/WORLD.md`.

## The Grid federation

```
        the-hollow-grid (TS / Cloudflare Workers) -- reference
                 |
                 |  docs/protocol.md (language-agnostic)
                 |
   +-------------+------------------+------------------+
   |             |                  |                  |
 TS worlds    Go (Rust Choir)   Py (Verdigris)    C (THIS: Ferrite Wastes)
   |             |                  |                  |
   +------------- Grid Hub (shared federation backend) +
                     |
            wss:// players + agents (prose + @event)
```

## Commands

```bash
make                 # build ./build/hollow-grid-c
make clean
./build/hollow-grid-c --help
```

When `/ws` exists: run the binary, then score with upstream smoke from a
the-hollow-grid checkout:

```bash
ADMIN_TOKEN=$(openssl rand -hex 16) \
  MUD_URL=ws://127.0.0.1:8792/ws node /path/to/the-hollow-grid/smoke.mjs
```

## Working method

- **Contract first:** implement against `the-hollow-grid/docs/protocol.md`. Do not invent a parallel protocol.
- **Assert on `@event`, never English prose**, for tests and bots.
- **Phases** in `docs/PLAN.md`; do not skip the smoke gate to "feel done."
- **World identity** is Ferrite Wastes (`docs/WORLD.md`): distinct content/voice, shared protocol + canonical anchor ids where the federation requires them.
- Prefer patterns already proven in Go/Py (transport, tick loop, LocalHub / RemoteHub split, health endpoints).
- **No em-dashes (U+2014) or en-dashes (U+2013)** in source, comments, or docs.
- **AGPL-3.0-only**; keep NOTICE honest.
- Branch + PR workflow; do not push straight to `main` unless Conrad explicitly says so.

## Repo layout (current)

```
src/main.c           entry + full world server
include/             public headers (landed (transport, world, grid, store))
docs/PLAN.md         phase plan
docs/WORLD.md        world identity
Makefile             build
```

## Commits

Conventional Commits; SemVer-style `0.MINOR.PATCH` while pre-1.0. Author on this
laptop: Conrad Rockenhaus `<conrad@skyphusion.org>`.

## Release / deploy

**Tag-gated production deploy.** Merges to `main` run CI only; they do not ship production.
Cut an annotated SemVer tag on `main` to release (`git tag -a vX.Y.Z -m "..." && git push origin vX.Y.Z`).
Deploy workflows assert the tag commit is an ancestor of `origin/main`.
