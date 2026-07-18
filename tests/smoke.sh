#!/bin/sh
# Blocking upstream smoke.mjs conformance against a local Ferrite Wastes.
# Requires: node >= 18, curl, and a checkout of the-hollow-grid (or SMOKE_MJS).
set -eu

binary=${1:-./build/hollow-grid-c}
port=${HG_SMOKE_PORT:-8792}
smoke_mjs=${SMOKE_MJS:-}
data=$(mktemp -d)
log=$(mktemp)
# Full suite can exceed a few minutes under CI load; give headroom and honor SMOKE_SLOW.
smoke_timeout=${HG_SMOKE_TIMEOUT:-600}

if [ -z "$smoke_mjs" ]; then
  for cand in \
    "${HG_SMOKE_DIR:-}/smoke.mjs" \
    "../the-hollow-grid/smoke.mjs" \
    "/home/conrad/dev/the-hollow-grid/smoke.mjs" \
    "$HOME/Documents/GitHub/the-hollow-grid/smoke.mjs" \
    "$HOME/dev/the-hollow-grid/smoke.mjs"
  do
    if [ -n "$cand" ] && [ -f "$cand" ]; then
      smoke_mjs=$cand
      break
    fi
  done
fi

if [ -z "$smoke_mjs" ] || [ ! -f "$smoke_mjs" ]; then
  echo "smoke.mjs not found; set SMOKE_MJS or clone the-hollow-grid" >&2
  exit 2
fi

"$binary" --addr "127.0.0.1:$port" --data "$data" \
  >"$log" 2>&1 &
pid=$!
cleanup() {
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  rm -f "$log"
  rm -rf "$data"
}
trap cleanup EXIT

attempt=0
while [ "$attempt" -lt 80 ]; do
  if curl -fsS "http://127.0.0.1:$port/health" >/dev/null 2>&1; then
    break
  fi
  attempt=$((attempt + 1))
  sleep 0.1
done
curl -fsS "http://127.0.0.1:$port/health" >/dev/null

export MUD_URL="ws://127.0.0.1:${port}/ws"
# Unreachable second world: Phase 12 must SKIP, not FAIL.
export DUSTFALL_URL="${DUSTFALL_URL:-ws://127.0.0.1:18788/ws}"
export SMOKE_SLOW="${SMOKE_SLOW:-2}"

echo "smoke: binary=$binary port=$port suite=$smoke_mjs timeout=${smoke_timeout}s SMOKE_SLOW=$SMOKE_SLOW"
timeout "$smoke_timeout" node "$smoke_mjs"
