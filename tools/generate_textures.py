#!/usr/bin/env python3
"""Generate the seamless procedural preview texture shipped with MaterialForge.

Usage:
    python tools/generate_textures.py

Requires Pillow and NumPy. The output is deterministic so regenerating it
does not produce noisy repository diffs.
"""

from __future__ import annotations

import math
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "assets" / "textures"
SIZE = 1024


def periodic_line_distance(coord: np.ndarray, spacing: float) -> np.ndarray:
    return np.abs((coord % spacing) - spacing * 0.5)


def glow(width: np.ndarray, falloff: float) -> np.ndarray:
    return np.exp(-(width * width) / (2.0 * falloff * falloff))


def make_holo_grid(size: int = SIZE) -> Image.Image:
    y, x = np.mgrid[0:size, 0:size].astype(np.float32)
    xx = x + 0.5
    yy = y + 0.5

    # Deep teal background with a very soft vertical gradient.
    base = np.zeros((size, size, 3), dtype=np.float32)
    gradient = yy / size
    base[..., 0] = 0.006 + 0.005 * gradient
    base[..., 1] = 0.015 + 0.010 * gradient
    base[..., 2] = 0.020 + 0.016 * gradient

    # Fine and major printed-circuit grids. Both are periodic, so the texture
    # tiles without a visible seam.
    minor = np.maximum(
        glow(periodic_line_distance(xx, 32.0), 0.9),
        glow(periodic_line_distance(yy, 32.0), 0.9),
    )
    major = np.maximum(
        glow(periodic_line_distance(xx, 128.0), 1.6),
        glow(periodic_line_distance(yy, 128.0), 1.6),
    )

    # Dashed diagonal traces. The break pattern is based on a periodic band so
    # it remains seamless when tiled.
    diagonal_coord = (xx + yy * 2.0) % 256.0
    diagonal_line = glow(np.abs(diagonal_coord - 64.0), 1.3)
    dash_coord = (xx * 0.35 + yy * 0.12) % 96.0
    dash = np.clip((96.0 - np.abs(dash_coord - 48.0)) / 8.0, 0.0, 1.0)
    diagonal = diagonal_line * (0.15 + 0.85 * dash)

    # Periodic hexagonal-ish pads at major intersections.
    dx = (xx % 256.0) - 96.0
    dy = (yy % 256.0) - 96.0
    radius = np.sqrt(dx * dx + dy * dy)
    pads = glow(np.abs(radius - 15.0), 1.2) * (radius < 30.0)
    pad_cores = glow(radius, 2.2) * 0.4

    # Slow arcs across the tile, repeated in a wrap-safe way.
    arc_phase = np.sqrt(
        ((xx - size * 0.25 + size * 0.5) % size - size * 0.75) ** 2
        + ((yy - size * 0.38) % size - size * 0.5) ** 2
    )
    arcs = glow(np.abs(arc_phase - 210.0), 2.0) * 0.25

    teal = np.array([0.0, 0.75, 0.82], dtype=np.float32)
    cyan = np.array([0.0, 0.95, 1.0], dtype=np.float32)
    pale = np.array([0.55, 1.0, 1.0], dtype=np.float32)
    amber = np.array([1.0, 0.42, 0.08], dtype=np.float32)

    image = base
    image += minor[..., None] * np.array([0.0, 0.10, 0.13], dtype=np.float32)
    image += major[..., None] * teal * 0.55
    image += diagonal[..., None] * cyan * 0.6
    image += pads[..., None] * pale * 0.9
    image += pad_cores[..., None] * cyan * 0.5
    image += arcs[..., None] * cyan * 0.35

    # Amber accents in a few periodic cells to break up the monochrome look.
    accent_mask = (((xx / 256.0).astype(np.int32) * 7 + (yy / 256.0).astype(np.int32) * 13) % 11 == 3)
    accent_line = glow(periodic_line_distance(xx, 256.0), 1.1) * accent_mask
    image += accent_line[..., None] * amber * 0.8

    # Subtle scanline modulation and grain.
    scanline = 0.94 + 0.06 * (0.5 + 0.5 * np.cos(yy * math.pi * 0.5))
    image *= scanline[..., None]
    rng = np.random.default_rng(0xC0FFEE)
    noise = rng.normal(0.0, 0.004, (size, size, 1)).astype(np.float32)
    image += noise

    image = np.clip(image, 0.0, 1.0)
    # Small contrast boost and a gentle bloom pass for the bright traces.
    image = np.clip((image - 0.5) * 1.12 + 0.5, 0.0, 1.0)
    bloom = np.clip(image - 0.35, 0.0, 1.0)
    # Cheap separable blur using repeated box filtering.
    for _ in range(3):
        bloom = (
            np.roll(bloom, 1, axis=0) + bloom + np.roll(bloom, -1, axis=0) +
            np.roll(bloom, 1, axis=1) + np.roll(bloom, -1, axis=1)
        ) / 5.0
    image = np.clip(image + bloom * 0.35, 0.0, 1.0)

    return Image.fromarray((image * 255.0 + 0.5).astype(np.uint8))


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    texture = make_holo_grid()
    path = OUT_DIR / "T_HoloGrid.png"
    texture.save(path, optimize=True)
    print(f"Wrote {path.relative_to(ROOT)} ({path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
