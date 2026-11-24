# hollow-grid-c

This file is the guide for any coding agent and any contributor. Follow it.

## What this project is

Ferrite Wastes is a Hollow Grid world server, written in C. It is a world node. It is not a Workers port. It speaks the same wire protocol. Set `GRID_HUB_URL` and it joins the Grid Hub.

The reference implementation is [the-hollow-grid](https://github.com/skyphusion-labs/the-hollow-grid). Sibling worlds are [hollow-grid-go](https://github.com/skyphusion-labs/hollow-grid-go) (Rust Choir) and [hollow-grid-py](https://github.com/skyphusion-labs/hollow-grid-py) (Verdigris Spool).

Protocol authority is `the-hollow-grid/docs/protocol.md`. Do not invent a parallel protocol. Definition of done is upstream `smoke.mjs`, scored against current the-hollow-grid head. Two consecutive stateful RemoteHub runs must pass. Do not freeze an old check count. Read [docs/PLAN.md](docs/PLAN.md) and [README.md](README.md) for the last recorded score.

The public host stays offline. A `v*` tag publishes the container image. The workflow has no host deploy step. See [docs/PLAN.md](docs/PLAN.md) and [docs/WORLD.md](docs/WORLD.md).

The license is AGPL-3.0-only. Keep [NOTICE](NOTICE) honest. See [LICENSE](LICENSE).

## Commands

```bash
make                 # build ./build/hollow-grid-c
make check           # build, make test, and --help
make clean
./build/hollow-grid-c --help
./build/hollow-grid-c --addr 127.0.0.1:8792
```

The default `LISTEN_ADDR` is `0.0.0.0:8792`. Players connect at `/ws`.

Score with upstream smoke from a the-hollow-grid checkout:

```bash
ADMIN_TOKEN=$(openssl rand -hex 16) \
  MUD_URL=ws://127.0.0.1:8792/ws node /path/to/the-hollow-grid/smoke.mjs
```

CI runs `make check` ([.github/workflows/ci.yml](.github/workflows/ci.yml)). [.github/workflows/release.yml](.github/workflows/release.yml) builds the image on every push and pull request. Upstream smoke in that workflow fails the job on a real failure. A `v*` tag pushes `ghcr.io/skyphusion-labs/hollow-grid-c` as `:<sha>` and `:latest`. A merge to `main` does not push the image.

## How to work

Implement against `the-hollow-grid/docs/protocol.md`. Assert on `@event`. Do not assert on English prose. Follow the phases in [docs/PLAN.md](docs/PLAN.md). Do not skip the smoke gate.

World identity is Ferrite Wastes. Read [docs/WORLD.md](docs/WORLD.md). Keep a distinct voice. Reuse the canonical anchor ids the federation requires.

Prefer patterns already proven in the Go port and the Python port. Copy their transport and their tick loop. Copy their LocalHub and RemoteHub split. Copy their health endpoints.

Use a branch and a pull request. Do not push straight to `main`.

Do not use an em dash (U+2014) or an en dash (U+2013) in source, comments, or docs. Use a comma, parentheses, or two hyphens (`--`).

## Repo layout

```
src/main.c              entry (flags, listen). Default port 8792.
src/transport/server.c  /ws and HTTP
src/world/              world and items
src/grid/remote.c       RemoteHub client
src/store/store.c       SQLite character store
src/event/event.c       @event framing
include/                public headers
docs/PLAN.md            phase plan
docs/WORLD.md           world identity
Makefile                build, test, check, smoke
tests/                  test_core.c, smoke.sh
```

## Docs

Write Simplified Technical English. Use short sentences. Put one idea in each sentence. Use the active present tense. Use plain words. Use the same word every time.

State the limit next to the capability. Do not turn a measured result into a promise. The public host stays offline until a host deploy exists. An image publish is not a host deploy.

## Commits and version

Use Conventional Commits. The body says why.

A version moves only in a release pull request. A feature pull request does not bump the version. A fix pull request does not bump the version. See the latest `v*` tag for the current version.

The tag form is `v*`. Put the tag on the merge commit of the release pull request. While the project is pre-1.0, use `0.MINOR.PATCH`.

Before a release pull request, look for a tag of the current version. A matching tag means the cycle is closed. The release pull request then bumps. No matching tag means a bump already landed outside a release pull request. Stop and read why.

## Release

Tag-gated image publish. No host deploy.

```bash
git tag -a vX.Y.Z -m "vX.Y.Z"
git push origin vX.Y.Z
```

`release.yml` pushes `:<sha>` and `:latest` on that tag. It does not check that the tag commit is an ancestor of `origin/main`. There is no host deploy step.

## Safety

Do not put a secret in a tracked file. Do not put a secret in a log. Do not put a secret in an issue. Keep `.env.example` free of live values.
