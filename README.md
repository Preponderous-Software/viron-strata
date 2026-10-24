# viron-strata

A live, read-only 3D view of a [Viron](https://github.com/Preponderous-Software/viron) environment, drawn with the [Strata](https://github.com/Preponderous-Software/strata) engine. Point it at a Viron URL, a token and an environment. It polls Viron's REST API several times a second and shows grids and entities as a driver (a simulation, a script, a game) changes them.

![A live Viron environment: two 10x10 grids, cubes stepping between cells as a driver script moves them, appearing, disappearing and jumping between grids](docs/media/live-random-walk.gif)

*Recorded headlessly while [`tools/driver.py`](tools/driver.py) changed a local Viron: 16 entities on two grids. Each one steps to a free neighbouring cell every tick. Every few ticks one is taken off the grid (it shrinks out), a new one is placed (it grows in), or one is sent to the other grid (it snaps instead of sliding).*

| Three grids, 59 entities, one cell picked | Viron stopped: the view says so and dims |
|---|---|
| ![Three 12x12 grids with 59 entities; a picked cell lists its entity's id and name](docs/media/live-three-grids.png) | ![The same world drawn dim with "stale for 5 s, unreachable" in the HUD](docs/media/stale.png) |

It implements RFC 0030 in [Preponderous-Software/rfcs](https://github.com/Preponderous-Software/rfcs). Its companion is [viron-usd](https://github.com/Preponderous-Software/viron-usd), which records a Viron run as OpenUSD for playback afterwards. viron-strata shows the run while it happens. The two use the same layout, so a live view and a recording of the same run line up.

## What it does

- **Read-only by construction.** The HTTP layer (`src/transport.h`) has one method, `get`, so the client has no way to send a write.
- **Light on Viron.** In the steady state each poll is a single request, `GET /api/v1/locations/placements?environmentId={id}`, which returns every placement from one consistent query. Grids are re-read every 20 polls and names every 40, and names are also fetched when a new entity id appears.
- **Smooth.** Each entity slides from its last polled cell to its new one over one poll period. A jump of more than one cell, or onto another grid, snaps. New entities grow in and removed ones shrink out.
- **Looks like the recording.** It uses viron-usd's conventions: one cell is one unit, grids sit side by side (`index × (rows + 2)`), and entities are 0.4 cubes. Each entity's offset within its cell and its colour both come from SHA-256 of its id. A conformance file generated from viron-usd's own `mapping.py` is checked in CI.
- **Honest about state.** The HUD says `live` (with the update age and poll latency), `Viron busy` (503 + `Retry-After`), `stale for N s` (5xx, timeout or refused: exponential backoff from 0.5 s to 10 s, with the last world kept on screen and dimmed), `token rejected` (401: it stops), `environment gone` (404: it stops), or that the Viron is too old to have `/locations/placements`. It also says the view is polled, not tick-exact.
- **Picking.** Click a cell to list the entities on it. Strata's raycast tests meshes, not primitives, so a click picks a cell rather than one cube.

## Build

CMake 3.24+, a C++17 compiler, libcurl (`libcurl4-openssl-dev`), and the X11/EGL development packages Strata's SDL3 build needs on Linux (see `.github/workflows/ci.yml`). Strata `v0.4.0` (pinned in `strata.ref`) and nlohmann/json 3.11.3 (pinned by SHA-256) are fetched at configure time.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

## Run

```
export VIRON_TOKEN=...            # a UserAuth JWT that Viron accepts
build/viron-strata --viron-url http://localhost:9999 --environment 4
build/viron-strata --environment "my world" --period-ms 100
```

`--environment` takes an id or a name. The token comes from `VIRON_TOKEN` or `--token`. These are the same names viron-usd uses, and `--url` is accepted for `--viron-url`. The token is never written to disk, logged or drawn. `https://` URLs are verified against the system's certificate store.

Window controls: drag or WASD/arrows pan, Q/E rotate, mouse wheel or +/- zoom, click a cell to list its entities, F re-frames, Tab toggles the HUD, Esc quits.

Headless (EGL surfaceless, so Mesa llvmpipe works without a display: `LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe`):

```
build/viron-strata --environment 4 --headless --duration 15 --fps 10 --frames out/ --screenshot last.png
build/viron-strata --environment 4 --headless --duration 2 --verify
```

`--verify` waits for a fresh poll at the end, then compares the client's world with an independent `GET /locations/placements`. If they differ, or an entity is not drawn on its cell, it exits with status 1.

## Trying it against a local Viron

[docs/LOCAL_VIRON.md](docs/LOCAL_VIRON.md) shows how to start a throwaway Viron, mint a token for it, and run [`tools/driver.py`](tools/driver.py), the demo driver that recorded the GIF above. The driver writes to Viron. viron-strata never does.

## Tests

- `[unit]`: the poll loop against a scripted fake Viron. That covers which requests each poll makes, steady-state cost, refreshing grids and names, the name fallback, 401, 404, 503 + `Retry-After`, backoff and recovery, an old Viron, and schema errors. It also covers tweening, snapping, growing and shrinking, and SHA-256 test vectors.
- `[conformance]`: offsets, colours, grid origins, cell centres, entity positions and floor face order, checked against `test/conformance/viron_usd_mapping.json`, which `tools/gen_conformance.py` generates from viron-usd's `mapping.py`. A CI job regenerates that file from viron-usd's `main` and fails if the conventions drift.
- `[render]` (EGL + GL 4.5): a fixed two-grid world compared with a golden image (16x10 block averages, so the test tolerates small rasteriser differences), picking cells on both grids, and batching more than 200 floors into one mesh.
- Integration (CI): a real Viron at a pinned commit on PostgreSQL 12. The driver changes it while the client watches, then `--verify` runs once the driver stops. A rejected token must stop the client.

## Not yet

- **A browser build.** Strata v0.4.0 can target WebGL 2, but Viron sends no CORS headers, and any Viron token can write, so a page holding one would hand out write access. RFC 0030 stages the browser view behind a Viron change: either a CORS allow-list or a same-origin proxy, plus a read-only token or scope. Those changes are tracked as [viron#244](https://github.com/Preponderous-Software/viron/issues/244) (CORS) and [viron#245](https://github.com/Preponderous-Software/viron/issues/245) (a read-only token).
- **Picking one cube** among several on a cell would need primitive raycasts in Strata, which would be a Strata change first.
- Push updates (SSE or WebSocket) and an `ETag` or revision counter on `/locations/placements` would make an idle world nearly free to watch. Both would be Viron changes.

## License

MIT. See [LICENSE](LICENSE).
