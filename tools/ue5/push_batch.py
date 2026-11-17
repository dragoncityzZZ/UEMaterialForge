#!/usr/bin/env python3
"""Batch MaterialForge -> UE5 push bridge.

The MaterialForge application can export a `ue_batch.json` manifest from
**Project > UE5 Push / Pull... > Export Project Batch**. Run this script inside
the Unreal Editor Python environment to import every material in that manifest:

    py "C:/path/to/tools/ue5/push_batch.py" "C:/path/to/ue_batch.json" \
       --texture-map "C:/path/to/tools/ue5/texture_map.json"
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))

import import_material_graph as importer  # noqa: E402


def warn(message: str) -> None:
    importer.warn(message)


def log(message: str) -> None:
    importer.log(message)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Batch push MaterialForge graphs into UE5.")
    parser.add_argument("manifest", help="ue_batch.json exported by MaterialForge.")
    parser.add_argument("--texture-map", default=None,
                        help="Optional texture map JSON shared by all materials.")
    parser.add_argument("--dest-path", default=None,
                        help="Override the manifest's UE destination folder.")
    parser.add_argument("--dry-run", action="store_true",
                        help="Validate all graphs without modifying UE assets.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    manifest_path = Path(args.manifest).expanduser().resolve()
    if not manifest_path.is_file():
        print(f"Batch manifest does not exist: {manifest_path}", file=sys.stderr)
        return 2
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001
        print(f"Could not read batch manifest: {exc}", file=sys.stderr)
        return 2
    if manifest.get("schema") != "materialforge.ue5.batch":
        print("Unsupported batch manifest schema.", file=sys.stderr)
        return 2

    dest_path = args.dest_path or manifest.get("destPath") or "/Game/MaterialForge/Materials"
    materials = [str(item) for item in manifest.get("materials", [])]
    if not materials:
        print("Batch manifest contains no materials.", file=sys.stderr)
        return 2

    failures = 0
    imported = 0
    for index, item in enumerate(materials):
        graph_path = Path(item)
        if not graph_path.is_absolute():
            graph_path = (manifest_path.parent / graph_path).resolve()
        if not graph_path.is_file():
            warn(f"[{index + 1}/{len(materials)}] Missing graph: {graph_path}")
            failures += 1
            continue
        try:
            document = importer.load_json(graph_path)
            importer.validate_document(document)
            texture_map = importer.load_texture_map(args.texture_map, graph_path)
            plan = importer.build_import_plan(document, graph_path, texture_map)
        except Exception as exc:  # noqa: BLE001
            warn(f"[{index + 1}/{len(materials)}] Invalid graph {graph_path.name}: {exc}")
            failures += 1
            continue

        material_name = plan.get("material_name", graph_path.stem)
        log(f"[{index + 1}/{len(materials)}] {graph_path.name} -> {dest_path}/{material_name}")
        for node_id, node_type, class_name in plan.get("unsupported", []):
            warn(f"Unsupported UE class for {node_type} (#{node_id}): {class_name}")
        for node_id, source in plan.get("unresolved_textures", []):
            warn(f"TextureSample #{node_id} is missing mapped texture: {source}")

        if args.dry_run:
            imported += 1
            continue
        if importer.unreal is None:
            print("Batch push requires the Unreal Editor Python environment.", file=sys.stderr)
            return 3
        try:
            import_args = SimpleNamespace(material_name=None, dest_path=dest_path)
            importer.import_material(import_args, document, plan)
            imported += 1
        except Exception as exc:  # noqa: BLE001
            warn(f"Import failed for {graph_path.name}: {exc}")
            failures += 1

    log(f"Batch complete: {imported} imported, {failures} failed.")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
