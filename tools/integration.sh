#!/usr/bin/env bash
# The integration test (RFC 0030, section 5): a driver changes a real Viron
# while viron-strata watches it headlessly; after the driver stops, the
# client's world must equal GET /locations/placements (--verify), and every
# entity must be drawn on its cell.
#
#   VIRON_URL=http://localhost:9999 VIRON_TOKEN=... [VIRON_READ_TOKEN=...] tools/integration.sh <build-dir> [frames-dir]
#
# The driver writes with VIRON_TOKEN. The client reads with VIRON_READ_TOKEN
# when it is set (a read-only token, scope viron:read), else VIRON_TOKEN.
#
# Needs a running Viron (docs/LOCAL_VIRON.md) and an EGL + GL 4.5 driver
# (Mesa llvmpipe: LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe).
set -euo pipefail
BUILD="${1:?build directory}"
FRAMES="${2:-}"
URL="${VIRON_URL:-http://localhost:9999}"
: "${VIRON_TOKEN:?VIRON_TOKEN must be set}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

TICKS=30
TPS=5
python3 "$HERE/driver.py" --viron-url "$URL" --name "integration-$$" --ticks "$TICKS" --tps "$TPS" \
  --start-delay 1.0 --env-file "$TMP/env" > "$TMP/driver.log" 2>&1 &
DRIVER=$!
for _ in $(seq 1 100); do [ -s "$TMP/env" ] && break; sleep 0.1; done
[ -s "$TMP/env" ] || { cat "$TMP/driver.log"; echo "driver did not create an environment"; exit 1; }
ENV_ID="$(cat "$TMP/env")"
echo "watching environment $ENV_ID while the driver runs"

CLIENT_TOKEN="${VIRON_READ_TOKEN:-$VIRON_TOKEN}"
ARGS=(--viron-url "$URL" --environment "$ENV_ID" --headless --duration 3 --width 480 --height 300)
if [ -n "$FRAMES" ]; then mkdir -p "$FRAMES"; ARGS+=(--frames "$FRAMES" --fps 5); fi
# First while the driver is writing (no --verify: the world is moving) ...
VIRON_TOKEN="$CLIENT_TOKEN" "$BUILD/viron-strata" "${ARGS[@]}"
wait "$DRIVER"
cat "$TMP/driver.log"
# ... then once it has stopped: the client must agree with Viron exactly.
VIRON_TOKEN="$CLIENT_TOKEN" "$BUILD/viron-strata" --viron-url "$URL" --environment "$ENV_ID" --headless --duration 2 --width 480 --height 300 \
  --screenshot "${FRAMES:-$TMP}/final.png" --verify
