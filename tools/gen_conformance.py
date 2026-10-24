"""Writes test/conformance/viron_usd_mapping.json from viron-usd's own mapping.py.

    python3 tools/gen_conformance.py <viron-usd checkout> [out.json]

It loads src/viron_usd/mapping.py from the checkout and records what it gives
for a fixed set of entity ids and grid cells, in viron-usd's axes (Z up). The
C++ test (test/test_mapping.cpp) converts to Strata's axes (Y up) and compares.
Only the pure layout functions are called; usd_recorder.values, which
mapping.py imports for driver extras, is stubbed so usd-core is not needed.
"""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import types
from pathlib import Path


def load_mapping(checkout: Path):
    stub = types.ModuleType("usd_recorder")
    values = types.ModuleType("usd_recorder.values")
    values.INT32_MIN, values.INT32_MAX = -(2**31), 2**31 - 1
    values.is_valid_property_name = values.value_type = values.normalize_color = lambda *a, **k: None
    stub.values = values
    sys.modules.setdefault("usd_recorder", stub)
    sys.modules.setdefault("usd_recorder.values", values)
    path = checkout / "src" / "viron_usd" / "mapping.py"
    spec = importlib.util.spec_from_file_location("viron_usd_mapping", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class G:
    def __init__(self, grid_id: int, rows: int, columns: int, index: int):
        self.grid_id, self.rows, self.columns, self.index = grid_id, rows, columns, index


def main() -> int:
    checkout = Path(sys.argv[1])
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parent.parent / "test/conformance/viron_usd_mapping.json"
    m = load_mapping(checkout)
    commit = subprocess.run(["git", "-C", str(checkout), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    ids = list(range(0, 64)) + [99, 100, 255, 256, 1000, 4096, 65535, 123456, 2**31 - 1, 2**40 + 7]
    grids = [G(1, 12, 12, 0), G(7, 5, 9, 1), G(9, 30, 4, 2)]
    doc = {
        "source": "Preponderous-Software/viron-usd src/viron_usd/mapping.py",
        "viron_usd_commit": commit,
        "axes": "viron-usd: Viron x -> +X, Viron y -> +Y, Z up",
        "constants": {"GRID_GAP": m.GRID_GAP, "ENTITY_SIZE": m.ENTITY_SIZE, "MAX_OFFSET": m.MAX_OFFSET},
        "entities": [
            {"id": i, "offset": list(m.entity_offset(i)), "color": list(m.entity_color(i))} for i in ids
        ],
        "cells": [],
        "positions": [],
    }
    for g in grids:
        for x, y in [(0, 0), (1, 0), (0, 1), (g.rows - 1, g.columns - 1), (g.rows // 2, g.columns // 3)]:
            doc["cells"].append({
                "grid": {"index": g.index, "rows": g.rows, "columns": g.columns},
                "x": x, "y": y,
                "origin": list(m.grid_origin(g)),
                "center": list(m.cell_center(g, x, y)),
                "face": m.face_index(g, x, y),
            })
            for e in (3, 41, 1000):
                doc["positions"].append({
                    "grid": {"index": g.index, "rows": g.rows, "columns": g.columns},
                    "x": x, "y": y, "id": e, "position": list(m.entity_position(g, x, y, e)),
                })
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(doc, indent=1) + "\n")
    print(f"wrote {out} ({len(doc['entities'])} ids, {len(doc['cells'])} cells, viron-usd {commit[:7]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
