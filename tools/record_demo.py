"""Records a Viron run for the page's demo mode (format viron-strata-demo/1).

    VIRON_TOKEN=<read-only token> python3 tools/record_demo.py --environment <id> \\
        --seconds 60 --out web/demo/recording.json

Polls the same read routes viron-strata polls, every --period-ms, while a
driver (tools/driver.py) changes the world, and writes what Viron answered:
the environment, its grids and its entities once, and the placements of
every poll as [entityId, gridId, locationId, x, y]. The page plays it back
through the client's own poll loop, so demo mode needs no server or token.

This script only reads (GET). Give it a read-only token (scope viron:read,
Viron's scripts/mint_read_token.py): it then cannot change the world it
records even by mistake. Standard library only (Python 3.8+).
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request


def get(base: str, token: str, path: str):
    req = urllib.request.Request(base.rstrip("/") + path, method="GET")
    req.add_header("Authorization", "Bearer " + token)
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read())


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--viron-url", default=os.environ.get("VIRON_URL", "http://localhost:9999"))
    p.add_argument("--environment", required=True, help="environment id")
    p.add_argument("--period-ms", type=int, default=250)
    p.add_argument("--seconds", type=float, default=60.0)
    p.add_argument("--hold-ms", type=int, default=1500, help="how long the last frame shows before the loop restarts")
    p.add_argument("--note", default="")
    p.add_argument("--out", required=True)
    a = p.parse_args()
    token = os.environ.get("VIRON_TOKEN")
    if not token:
        print("record_demo: VIRON_TOKEN must be set", file=sys.stderr)
        return 2
    env_id = int(a.environment)
    base = a.viron_url
    frames = []
    period = a.period_ms / 1000.0
    deadline = time.monotonic() + a.seconds
    next_at = time.monotonic()
    while time.monotonic() < deadline:
        placements = get(base, token, f"/api/v1/locations/placements?environmentId={env_id}")
        frames.append([[p["entityId"], p["gridId"], p["locationId"], p["x"], p["y"]] for p in placements])
        next_at += period
        time.sleep(max(0.0, next_at - time.monotonic()))
    # Trim the idle frames at the start (before the driver placed anything).
    while frames and not frames[0]:
        frames.pop(0)
    if not frames:
        print("record_demo: nothing was ever placed", file=sys.stderr)
        return 1
    environment = get(base, token, f"/api/v1/environments/{env_id}")
    grids = get(base, token, f"/api/v1/grids/environment/{env_id}")
    entities = {e["entityId"]: e for e in get(base, token, f"/api/v1/entities/environment/{env_id}")}
    for frame in frames:
        for entity_id, *_ in frame:
            if entity_id not in entities:
                try:
                    entities[entity_id] = get(base, token, f"/api/v1/entities/{entity_id}")
                except urllib.error.HTTPError:
                    entities[entity_id] = {"entityId": entity_id, "name": "", "creationDate": None}
    out = {
        "format": "viron-strata-demo/1",
        "note": a.note,
        "periodMs": a.period_ms,
        "holdMs": a.hold_ms,
        "environment": environment,
        "grids": sorted(grids, key=lambda g: g["gridId"]),
        "entities": [entities[k] for k in sorted(entities)],
        "frames": frames,
    }
    with open(a.out, "w") as f:
        json.dump(out, f, separators=(",", ":"))
        f.write("\n")
    print(f"record_demo: {len(frames)} frames, {len(entities)} entities, {len(grids)} grids -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
