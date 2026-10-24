"""A demo driver: changes a Viron world so viron-strata has something to watch.

    VIRON_TOKEN=... python3 tools/driver.py --ticks 120 --tps 4 [--viron-url URL]

**This script writes to Viron** (POST, PUT, DELETE). It stands in for a
simulation. viron-strata itself only reads. Run it only against a Viron you
started yourself (see docs/LOCAL_VIRON.md).

It creates an environment ("--name", default "strata-demo") with --grids grids
of --grid-size cells, creates --entities entities and places each on a random
free cell, then each tick moves every entity to a random free neighbouring
cell. Every few ticks it also takes one entity off the grid, places a new one,
or sends one to another grid, so appearing, disappearing and snapping show.
The first line printed is "environment <id>".

Standard library only (Python 3.8+), so CI needs nothing installed.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import sys
import time
import urllib.error
import urllib.request


class Viron:
    def __init__(self, base: str, token: str):
        self.base = base.rstrip("/")
        self.token = token

    def call(self, method: str, path: str, body=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.base + path, data=data, method=method)
        req.add_header("Authorization", "Bearer " + self.token)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                raw = resp.read()
                return json.loads(raw) if raw else None
        except urllib.error.HTTPError as e:
            raise RuntimeError(f"{method} {path} -> {e.code}: {e.read()[:200]!r}") from None


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--viron-url", default=os.environ.get("VIRON_URL", "http://localhost:9999"))
    p.add_argument("--token", default=os.environ.get("VIRON_TOKEN"))
    p.add_argument("--name", default="strata-demo")
    p.add_argument("--grids", type=int, default=2)
    p.add_argument("--grid-size", type=int, default=10)
    p.add_argument("--entities", type=int, default=16)
    p.add_argument("--ticks", type=int, default=120)
    p.add_argument("--tps", type=float, default=4.0, help="ticks per second (default 4)")
    p.add_argument("--seed", type=int, default=7)
    p.add_argument("--start-delay", type=float, default=0.0, help="seconds to wait after creating the world")
    p.add_argument("--env-file", default=None, help="also write the environment id here")
    a = p.parse_args()
    if not a.token:
        print("driver: no token (--token or VIRON_TOKEN)", file=sys.stderr)
        return 2
    rng = random.Random(a.seed)
    v = Viron(a.viron_url, a.token)

    env = v.call("POST", "/api/v1/environments", {"name": a.name, "numGrids": a.grids, "gridSize": a.grid_size})
    env_id = int(env["environmentId"])
    print(f"environment {env_id}", flush=True)
    if a.env_file:
        with open(a.env_file, "w") as f:
            f.write(f"{env_id}\n")
    grids = sorted(int(g["gridId"]) for g in v.call("GET", f"/api/v1/grids/environment/{env_id}"))
    cells = {}  # location id -> grid id
    for g in grids:
        for loc in v.call("GET", f"/api/v1/locations/grid/{g}"):
            cells[int(loc["locationId"])] = g
    position = {}  # entity id -> location id
    neighbours = {}
    counter = [0]

    def free(grid=None):
        taken = set(position.values())
        return [c for c in sorted(cells) if c not in taken and (grid is None or cells[c] == grid)]

    def spawn(grid=None):
        counter[0] += 1
        e = int(v.call("POST", "/api/v1/entities", {"name": f"walker-{counter[0]}"})["entityId"])
        target = rng.choice(free(grid))
        v.call("PUT", f"/api/v1/locations/{target}/entity/{e}")
        position[e] = target
        return e

    def near(loc):
        if loc not in neighbours:
            neighbours[loc] = [int(n["locationId"]) for n in v.call("GET", f"/api/v1/locations/{loc}/neighbors")]
        return neighbours[loc]

    for i in range(a.entities):
        spawn(grids[i % len(grids)])
    if a.start_delay > 0:
        time.sleep(a.start_delay)

    period = 1.0 / a.tps
    next_tick = time.monotonic()
    for tick in range(1, a.ticks + 1):
        for e in sorted(position):
            taken = set(position.values())
            options = [n for n in near(position[e]) if n not in taken]
            if options:
                target = rng.choice(options)
                v.call("PUT", f"/api/v1/locations/{target}/entity/{e}/move")
                position[e] = target
        if tick % 10 == 3 and position:
            gone = rng.choice(sorted(position))
            v.call("DELETE", f"/api/v1/locations/entity/{gone}")
            del position[gone]
        if tick % 10 == 7:
            spawn()
        if tick % 15 == 11 and len(grids) > 1 and position:
            # Send one entity to another grid: it snaps instead of sliding.
            e = rng.choice(sorted(position))
            other = rng.choice([g for g in grids if g != cells[position[e]]])
            v.call("DELETE", f"/api/v1/locations/entity/{e}")
            del position[e]
            target = rng.choice(free(other))
            v.call("PUT", f"/api/v1/locations/{target}/entity/{e}")
            position[e] = target
        next_tick += period
        delay = next_tick - time.monotonic()
        if delay > 0:
            time.sleep(delay)
    print(f"driver: {a.ticks} ticks done, {len(position)} entities placed", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
