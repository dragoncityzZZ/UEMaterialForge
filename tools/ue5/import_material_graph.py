#!/usr/bin/env python3
"""MaterialForge -> Unreal Engine 5 material import bridge.

The script reads a MaterialForge JSON document and rebuilds it with
`unreal.MaterialEditingLibrary` inside an Unreal Engine 5 editor Python
environment. It is intentionally data-driven: node classes come from the
JSON's `ueClass` field, so newly added MaterialForge expressions that have a
UE counterpart can be imported without touching the mapping table.

Typical usage in the UE5 editor's Python console / Output Log:

    py "C:/path/to/tools/ue5/import_material_graph.py" \
       "C:/path/to/examples/graphs/hologram_circuit.json" \
       --dest-path /Game/MaterialForge/Materials \
       --texture-map "C:/path/to/tools/ue5/texture_map.example.json"

Or from a command line with the UE editor closed (development only):

    python tools/ue5/import_material_graph.py graph.json --dry-run

Texture assets are not embedded in the material graph JSON. Provide a
texture-map JSON whose keys are MaterialForge texture paths (or file names)
and whose values are UE asset paths. The importer also tries a few conventional
UE paths before giving up and logs a warning.

Important compatibility notes:
  * The JSON pin names are the MaterialForge pin names. The importer queries
    the actual UE input names and maps the common aliases.
  * UE's Fresnel exponent is an editor property, not an input pin. MaterialForge
    matches that convention.
  * TextureObject nodes are resolved into the TextureSample.texture property,
    because the TextureSample asset picker is the canonical UE representation.
  * UVs, math operators, parameters and material outputs map one-to-one.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any, Optional

try:
    import unreal  # type: ignore
except ImportError:  # Allows --dry-run outside the editor.
    unreal = None  # type: ignore


SCHEMA_ID = "materialforge.ue5.material"
SCHEMA_VERSION = 1

MATERIAL_PROPERTIES = {
    "Base Color": "MP_BASE_COLOR",
    "Metallic": "MP_METALLIC",
    "Specular": "MP_SPECULAR",
    "Roughness": "MP_ROUGHNESS",
    "Emissive Color": "MP_EMISSIVE_COLOR",
    "Normal": "MP_NORMAL",
    "Ambient Occlusion": "MP_AMBIENT_OCCLUSION",
    "Opacity": "MP_OPACITY",
    "Opacity Mask": "MP_OPACITY_MASK",
    "World Position Offset": "MP_WORLD_POSITION_OFFSET",
    "Pixel Depth Offset": "MP_PIXEL_DEPTH_OFFSET",
    "Refraction": "MP_REFRACTION",
    "Clear Coat": "MP_CLEAR_COAT",
    "Clear Coat Roughness": "MP_CLEAR_COAT_ROUGHNESS",
    "Subsurface Color": "MP_SUBSURFACE_COLOR",
    "Anisotropy": "MP_ANISOTROPY",
}

BLEND_MODES = {
    "Opaque": "BLEND_OPAQUE",
    "Masked": "BLEND_MASKED",
    "Translucent": "BLEND_TRANSLUCENT",
    "Additive": "BLEND_ADDITIVE",
    "Modulate": "BLEND_MODULATE",
    "AlphaComposite": "BLEND_ALPHA_COMPOSITE",
}

MATERIAL_DOMAINS = {
    "Surface": "MD_SURFACE",
    "Deferred Decal": "MD_DEFERRED_DECAL",
    "Light Function": "MD_LIGHT_FUNCTION",
    "Volume": "MD_VOLUME",
    "Post Process": "MD_POST_PROCESS",
}

SHADING_MODELS = {
    "DefaultLit": "MSM_DEFAULT_LIT",
    "Unlit": "MSM_UNLIT",
    "Subsurface": "MSM_SUBSURFACE",
    "SubsurfaceProfile": "MSM_SUBSURFACE_PROFILE",
    "TwoSidedFoliage": "MSM_TWO_SIDED_FOLIAGE",
    "Hair": "MSM_HAIR",
    "Cloth": "MSM_CLOTH",
    "Eye": "MSM_EYE",
    "ClearCoat": "MSM_CLEAR_COAT",
}

TRANSLUCENCY_LIGHTING_MODES = {
    "SurfaceForwardShading": "TLM_SURFACE_FORWARD_SHADING",
    "SurfaceTranslucencyVolume": "TLM_SURFACE_TRANSLUCENCY_VOLUME",
    "VolumetricNonDirectional": "TLM_VOLUMETRIC_NON_DIRECTIONAL",
    "VolumetricDirectional": "TLM_VOLUMETRIC_DIRECTIONAL",
    "VolumetricPerVertexNonDirectional": "TLM_VOLUMETRIC_PER_VERTEX_NON_DIRECTIONAL",
    "VolumetricPerVertexDirectional": "TLM_VOLUMETRIC_PER_VERTEX_DIRECTIONAL",
}

TEXTURE_OUTPUT_NAMES = {"RGBA", "RGB", "R", "G", "B", "A"}


def log(message: str) -> None:
    if unreal is not None:
        unreal.log(f"[MaterialForge] {message}")
    else:
        print(f"[MaterialForge] {message}")


def warn(message: str) -> None:
    if unreal is not None:
        unreal.log_warning(f"[MaterialForge] {message}")
    else:
        print(f"[MaterialForge] WARNING: {message}", file=sys.stderr)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Import a MaterialForge material graph into UE5.")
    parser.add_argument("graph", help="Path to the MaterialForge JSON graph.")
    parser.add_argument("--dest-path", default="/Game/MaterialForge/Materials",
                        help="UE content path for the created material.")
    parser.add_argument("--material-name", default=None,
                        help="Override the material asset name.")
    parser.add_argument("--texture-map", default=None,
                        help="Optional JSON mapping MaterialForge texture paths to UE asset paths.")
    parser.add_argument("--dry-run", action="store_true",
                        help="Validate and print the import plan without changing the UE project.")
    return parser.parse_args()


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def sanitize_asset_name(name: str) -> str:
    cleaned = "".join(ch if ch.isalnum() or ch in "_-." else "_" for ch in name.strip())
    return cleaned or "M_MaterialForge"


def normalize_lookup_key(value: str) -> str:
    return value.replace("\\", "/").strip().lower()


def load_texture_map(explicit_path: Optional[str], graph_path: Path) -> dict[str, str]:
    candidates = []
    if explicit_path:
        candidates.append(Path(explicit_path))
    candidates.append(graph_path.parent / "texture_map.json")

    for candidate in candidates:
        if not candidate.is_file():
            continue
        try:
            payload = load_json(candidate)
        except Exception as exc:  # noqa: BLE001 - report and continue
            warn(f"Could not read texture map {candidate}: {exc}")
            continue
        if not isinstance(payload, dict):
            warn(f"Texture map {candidate} must contain a JSON object.")
            continue
        mapping: dict[str, str] = {}
        for key, value in payload.items():
            if isinstance(key, str) and isinstance(value, str):
                mapping[normalize_lookup_key(key)] = value
        log(f"Loaded {len(mapping)} texture mapping(s) from {candidate}.")
        return mapping
    return {}


def find_texture_asset(source_path: str, texture_map: dict[str, str]) -> Any:
    if unreal is None:
        return None

    normalized = normalize_lookup_key(source_path)
    file_name = Path(source_path).name
    stem = Path(source_path).stem
    keys = [
        normalized,
        normalize_lookup_key(file_name),
        normalize_lookup_key(stem),
    ]
    ue_path = None
    for key in keys:
        if key in texture_map:
            ue_path = texture_map[key]
            break
    if ue_path is None:
        for candidate in (
            f"/Game/MaterialForge/Textures/{stem}",
            f"/Game/Textures/{stem}",
            f"/Game/{stem}",
        ):
            asset = unreal.load_asset(candidate)
            if asset is not None:
                ue_path = candidate
                break

    if ue_path is None:
        warn(f"No UE texture asset mapping for '{source_path}'. "
             "Add it to the texture map JSON and re-run, or set the texture manually.")
        return None

    asset = unreal.load_asset(ue_path)
    if asset is None:
        warn(f"Texture map points to a missing asset: {ue_path}")
    return asset


def set_editor_property_safe(target: Any, property_name: str, value: Any) -> None:
    try:
        target.set_editor_property(property_name, value)
    except Exception as exc:  # noqa: BLE001 - UE property APIs are version-sensitive
        warn(f"Could not set {type(target).__name__}.{property_name}: {exc}")


def apply_node_properties(expression: Any, node: dict[str, Any]) -> None:
    node_type = node.get("type", "")
    properties = node.get("properties", {}) or {}

    if node_type == "Constant":
        set_editor_property_safe(expression, "r", float(properties.get("value", 0.0)))
    elif node_type == "Constant2Vector":
        values = properties.get("value", [0.0, 0.0])
        set_editor_property_safe(expression, "r", float(values[0]))
        set_editor_property_safe(expression, "g", float(values[1]))
    elif node_type in ("Constant3Vector", "Constant4Vector", "VectorParameter"):
        values = properties.get("value", [0.0, 0.0, 0.0, 1.0])
        if len(values) == 3:
            values = values + [1.0]
        color = unreal.LinearColor(values[0], values[1], values[2], values[3])
        if node_type == "VectorParameter":
            set_editor_property_safe(expression, "parameter_name",
                                     unreal.Name(properties.get("parameterName", "Color")))
            set_editor_property_safe(expression, "default_value", color)
        else:
            set_editor_property_safe(expression, "constant", color)
    elif node_type == "ScalarParameter":
        set_editor_property_safe(expression, "parameter_name",
                                 unreal.Name(properties.get("parameterName", "Value")))
        set_editor_property_safe(expression, "default_value", float(properties.get("value", 0.0)))
    elif node_type == "TextureCoordinate":
        set_editor_property_safe(expression, "u_tiling", float(properties.get("uTiling", 1.0)))
        set_editor_property_safe(expression, "v_tiling", float(properties.get("vTiling", 1.0)))
    elif node_type == "Panner":
        speed = properties.get("speed", [0.05, 0.02])
        set_editor_property_safe(expression, "speed_x", float(speed[0]))
        set_editor_property_safe(expression, "speed_y", float(speed[1]))
    elif node_type == "Rotator":
        center = properties.get("center", [0.5, 0.5])
        set_editor_property_safe(expression, "center_x", float(center[0]))
        set_editor_property_safe(expression, "center_y", float(center[1]))
        set_editor_property_safe(expression, "speed", float(properties.get("speed", 0.1)))
    elif node_type == "Fresnel":
        set_editor_property_safe(expression, "exponent", float(properties.get("exponent", 5.0)))
        set_editor_property_safe(expression, "base_reflect_fraction",
                                 float(properties.get("baseReflectFraction", 0.04)))
    # Other expressions do not have MaterialForge-editable properties.


def get_ue_input_names(expression: Any) -> list[str]:
    if unreal is None:
        return []
    try:
        names = unreal.MaterialEditingLibrary.get_material_expression_input_names(expression)
        return [str(name) for name in names]
    except Exception as exc:  # noqa: BLE001
        warn(f"Could not query input names for {type(expression).__name__}: {exc}")
        return []


def resolve_input_name(expression: Any, requested: str) -> Optional[str]:
    names = get_ue_input_names(expression)
    if not names:
        return requested or None

    lowered = {name.lower(): name for name in names}
    if requested.lower() in lowered:
        return lowered[requested.lower()]

    aliases = {
        "uvs": ("uvs", "coordinates", "uv"),
        "uv": ("uv", "uvs", "coordinates"),
        "coordinate": ("coordinate", "coordinates", "uvs", "uv"),
        "texture": ("texture", "textureobject", "tex"),
        "input": ("input", "value"),
        "value": ("value", "input"),
        "time": ("time",),
        "a": ("a",),
        "b": ("b",),
    }
    for alias in aliases.get(requested.lower(), ()): 
        if alias in lowered:
            return lowered[alias]
    if len(names) == 1:
        return names[0]
    return None


def resolve_output_name(node: dict[str, Any], pin_name: str) -> str:
    # Most UE expressions have one unnamed output. TextureSample exposes RGB and
    # channel outputs, which are already named the same way in MaterialForge.
    if node.get("type") == "TextureSample" and pin_name in TEXTURE_OUTPUT_NAMES:
        return pin_name
    return ""


def find_texture_source(node_id: int, links: list[dict[str, Any]], nodes_by_id: dict[int, dict[str, Any]]) -> str:
    for link in links:
        if link.get("toNode") == node_id and link.get("toPinName") == "Texture":
            source = nodes_by_id.get(link.get("fromNode", -1))
            if source and source.get("type") == "TextureObject":
                return str((source.get("properties") or {}).get("texture", ""))
    return ""


def set_material_settings(material: Any, settings: dict[str, Any]) -> None:
    blend = BLEND_MODES.get(settings.get("blendMode", "Opaque"), "BLEND_OPAQUE")
    shading = SHADING_MODELS.get(settings.get("shadingModel", "DefaultLit"), "MSM_DEFAULT_LIT")
    domain = MATERIAL_DOMAINS.get(settings.get("domain", "Surface"), "MD_SURFACE")
    blend_value = getattr(unreal.BlendMode, blend, None) if unreal else None
    shading_value = getattr(unreal.MaterialShadingModel, shading, None) if unreal else None
    domain_value = getattr(unreal.MaterialDomain, domain, None) if unreal else None
    if domain_value is not None:
        set_editor_property_safe(material, "material_domain", domain_value)
    if blend_value is not None:
        set_editor_property_safe(material, "blend_mode", blend_value)
    if shading_value is not None:
        set_editor_property_safe(material, "shading_model", shading_value)
    set_editor_property_safe(material, "two_sided", bool(settings.get("twoSided", False)))
    set_editor_property_safe(
        material, "opacity_mask_clip_value",
        float(settings.get("opacityMaskClip", 0.333)))
    set_editor_property_safe(
        material, "dithered_lod_transition",
        bool(settings.get("ditheredLodTransition", False)))
    set_editor_property_safe(
        material, "cast_dynamic_shadow_as_masked",
        bool(settings.get("castDynamicShadowAsMasked", False)))
    set_editor_property_safe(
        material, "use_emissive_for_static_lighting",
        bool(settings.get("useEmissiveForStaticLighting", False)))
    translucency = TRANSLUCENCY_LIGHTING_MODES.get(
        settings.get("translucencyLightingMode", "SurfaceForwardShading"),
        "TLM_SURFACE_FORWARD_SHADING")
    translucency_value = getattr(unreal.TranslucencyLightingMode, translucency, None) if unreal else None
    if translucency_value is not None:
        set_editor_property_safe(material, "translucency_lighting_mode", translucency_value)


def validate_document(document: dict[str, Any]) -> None:
    if document.get("schema") != SCHEMA_ID:
        raise ValueError(f"Unsupported schema '{document.get('schema')}' (expected '{SCHEMA_ID}').")
    if document.get("version") != SCHEMA_VERSION:
        raise ValueError(f"Unsupported schema version {document.get('version')}.")
    if not isinstance(document.get("nodes"), list) or not isinstance(document.get("links"), list):
        raise ValueError("Document must contain nodes and links arrays.")
    material_outputs = [node for node in document["nodes"] if node.get("type") == "MaterialOutput"]
    if len(material_outputs) != 1:
        raise ValueError("A material document must have exactly one MaterialOutput node.")


def build_import_plan(document: dict[str, Any], graph_path: Path,
                      texture_map: dict[str, str]) -> dict[str, Any]:
    nodes = document["nodes"]
    links = document["links"]
    nodes_by_id = {int(node["id"]): node for node in nodes}
    texture_samples = {}
    for node in nodes:
        if node.get("type") == "TextureSample":
            source = find_texture_source(int(node["id"]), links, nodes_by_id)
            texture_samples[int(node["id"])] = source

    unsupported = []
    for node in nodes:
        if node.get("type") in ("MaterialOutput", "TextureObject", "Comment"):
            continue
        ue_class_name = node.get("ueClass")
        if not ue_class_name or (unreal is not None and not hasattr(unreal, ue_class_name)):
            unsupported.append((node.get("id"), node.get("type"), ue_class_name))

    unresolved_textures = []
    for node_id, source in texture_samples.items():
        if source and unreal is not None and find_texture_asset(source, texture_map) is None:
            unresolved_textures.append((node_id, source))

    return {
        "nodes_by_id": nodes_by_id,
        "texture_samples": texture_samples,
        "unsupported": unsupported,
        "unresolved_textures": unresolved_textures,
        "material_name": sanitize_asset_name(document.get("material", {}).get("name", "M_MaterialForge")),
        "graph_path": graph_path,
    }


def import_material(args: argparse.Namespace, document: dict[str, Any], plan: dict[str, Any]) -> None:
    assert unreal is not None
    material_settings = document.get("material", {})
    material_name = sanitize_asset_name(args.material_name or plan["material_name"])
    dest_path = args.dest_path.rstrip("/")
    asset_path = f"{dest_path}/{material_name}"
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    if hasattr(unreal.EditorAssetLibrary, "make_directory"):
        unreal.EditorAssetLibrary.make_directory(dest_path)

    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        material = unreal.EditorAssetLibrary.load_asset(asset_path)
        if not isinstance(material, unreal.Material):
            raise TypeError(f"{asset_path} exists but is not a Material.")
        unreal.MaterialEditingLibrary.delete_all_material_expressions(material)
        log(f"Updating existing material {asset_path}.")
    else:
        material = asset_tools.create_asset(material_name, dest_path, unreal.Material,
                                            unreal.MaterialFactoryNew())
        if material is None:
            raise RuntimeError(f"Could not create material at {asset_path}.")
        log(f"Created material {asset_path}.")

    set_material_settings(material, material_settings)

    nodes = document["nodes"]
    links = document["links"]
    nodes_by_id = plan["nodes_by_id"]
    expressions: dict[int, Any] = {}

    # Texture objects are represented by the TextureSample asset property.
    for node in nodes:
        if node.get("type") in ("MaterialOutput", "TextureObject", "Comment"):
            continue
        node_id = int(node["id"])
        class_name = node.get("ueClass")
        expression_class = getattr(unreal, class_name, None) if class_name else None
        if expression_class is None:
            warn(f"Skipping {node.get('type')} (#{node_id}): no UE class '{class_name}'.")
            continue

        position = node.get("position", {})
        expression = unreal.MaterialEditingLibrary.create_material_expression(
            material,
            expression_class,
            int(round(float(position.get("x", 0.0)))),
            int(round(float(position.get("y", 0.0)))),
        )
        if expression is None:
            warn(f"UE did not create {node.get('type')} (#{node_id}).")
            continue
        apply_node_properties(expression, node)
        expressions[node_id] = expression

    # Resolve textures into TextureSample nodes.
    for node_id, source in plan["texture_samples"].items():
        expression = expressions.get(node_id)
        if expression is None or not source:
            continue
        texture_asset = find_texture_asset(source, texture_map)
        if texture_asset is not None:
            set_editor_property_safe(expression, "texture", texture_asset)

    # Connections between expressions.
    for link in links:
        from_node_id = int(link.get("fromNode", -1))
        to_node_id = int(link.get("toNode", -1))
        from_node = nodes_by_id.get(from_node_id)
        to_node = nodes_by_id.get(to_node_id)
        if not from_node or not to_node:
            warn(f"Skipping link with missing node: {link}")
            continue
        if from_node.get("type") == "TextureObject":
            continue  # Already applied to TextureSample.texture.

        source_expression = expressions.get(from_node_id)
        if source_expression is None:
            warn(f"Skipping link from unsupported node #{from_node_id} ({from_node.get('type')}).")
            continue
        output_name = resolve_output_name(from_node, str(link.get("fromPinName", "")))

        if to_node.get("type") == "MaterialOutput":
            property_name = MATERIAL_PROPERTIES.get(str(link.get("toPinName", "")))
            if property_name is None:
                warn(f"No UE material property mapping for '{link.get('toPinName')}'.")
                continue
            property_enum = getattr(unreal.MaterialProperty, property_name, None)
            if property_enum is None:
                warn(f"UE doesn't expose MaterialProperty.{property_name}; set this pin manually.")
                continue
            if not unreal.MaterialEditingLibrary.connect_material_property(
                    source_expression, output_name, property_enum):
                warn(f"Could not connect #{from_node_id}.{output_name} -> {property_name}.")
            continue

        destination_expression = expressions.get(to_node_id)
        if destination_expression is None:
            warn(f"Skipping link to unsupported node #{to_node_id}.")
            continue
        input_name = resolve_input_name(destination_expression, str(link.get("toPinName", "")))
        if not input_name:
            warn(f"Could not resolve input '{link.get('toPinName')}' on "
                 f"{to_node.get('type')} (#{to_node_id}).")
            continue
        if not unreal.MaterialEditingLibrary.connect_material_expressions(
                source_expression, output_name, destination_expression, input_name):
            warn(f"Could not connect #{from_node_id} -> #{to_node_id}.{input_name}.")

    try:
        unreal.MaterialEditingLibrary.recompile_material(material)
    except Exception as exc:  # noqa: BLE001
        warn(f"Material compile request failed: {exc}")
    try:
        unreal.EditorAssetLibrary.save_asset(asset_path, only_if_is_dirty=False)
    except Exception as exc:  # noqa: BLE001
        warn(f"Could not save {asset_path}: {exc}")
    log(f"Imported {len(expressions)} expressions, {len(links)} links into {asset_path}.")
    log(f"Open {asset_path} in the Material Editor to inspect the result.")


def main() -> int:
    args = parse_args()
    graph_path = Path(args.graph).expanduser().resolve()
    if not graph_path.is_file():
        warn(f"Graph file does not exist: {graph_path}")
        return 2

    try:
        document = load_json(graph_path)
        validate_document(document)
    except Exception as exc:  # noqa: BLE001
        warn(f"Invalid MaterialForge document: {exc}")
        return 2

    texture_map = load_texture_map(args.texture_map, graph_path)
    plan = build_import_plan(document, graph_path, texture_map)

    log(f"Graph: {graph_path}")
    log(f"Material: {plan['material_name']}")
    log(f"Nodes: {len(document['nodes'])}, links: {len(document['links'])}, "
        f"texture samples: {len(plan['texture_samples'])}")
    for node_id, node_type, class_name in plan["unsupported"]:
        warn(f"Unsupported UE class for {node_type} (#{node_id}): {class_name}")
    for node_id, source in plan["unresolved_textures"]:
        warn(f"TextureSample #{node_id} is missing mapped texture: {source}")

    if args.dry_run:
        log("Dry run complete; no UE assets were modified.")
        return 0

    if unreal is None:
        warn("The 'unreal' module is only importable inside the UE5 editor Python environment.")
        warn("Run this script from the UE Output Log, or use --dry-run outside the editor.")
        return 3

    try:
        import_material(args, document, plan)
    except Exception as exc:  # noqa: BLE001
        warn(f"Import failed: {exc}")
        raise
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
