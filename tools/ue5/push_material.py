#!/usr/bin/env python3
"""MaterialForge -> Unreal Engine 5 push entry point.

This is a thin wrapper around `import_material_graph.py` so the UE bridge has
symmetric `push_material.py` / `pull_material_graph.py` entry points.

Inside the UE5 editor Python console:

    py "C:/path/to/tools/ue5/push_material.py" \
       "C:/path/to/M_Example.json" \
       --dest-path /Game/MaterialForge/Materials \
       --texture-map "C:/path/to/tools/ue5/texture_map.json"

All arguments are forwarded to the importer unchanged.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import import_material_graph  # noqa: E402


if __name__ == "__main__":
    raise SystemExit(import_material_graph.main())
