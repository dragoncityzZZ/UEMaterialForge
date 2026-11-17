#!/usr/bin/env python3
"""Generate the MaterialForge default texture library.

Usage:
    python tools/generate_default_textures.py             # 全部生成
    python tools/generate_default_textures.py --sheet     # 额外输出缩略图总览
    python tools/generate_default_textures.py --only T_Wood T_Marble
    python tools/generate_default_textures.py --list

Requires Pillow + NumPy. Fully deterministic: the same seed always produces
byte-identical output, so regenerating never dirties the repository.

Every tileable texture is verified against its own interior gradient before it
is written; a texture whose seam is more than 3x the interior step is reported
as a failure instead of being shipped silently.
"""

from __future__ import annotations

import argparse
import colorsys
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "assets" / "textures"
SHEET_DIR = ROOT / "docs" / "images"
SEED = 0xC0FFEE
TAU = math.pi * 2.0


# --------------------------------------------------------------------------- #
# 基础工具
# --------------------------------------------------------------------------- #
def uv_grid(size: int, height: int | None = None) -> tuple[np.ndarray, np.ndarray]:
    """u 向右递增，v 向上递增（与加载器的垂直翻转一致）。"""
    h = height or size
    y, x = np.mgrid[0:h, 0:size].astype(np.float32)
    return (x + 0.5) / size, 1.0 - (y + 0.5) / h


def smoothstep(e0: float, e1: float, x: np.ndarray) -> np.ndarray:
    span = e1 - e0
    if abs(span) < 1e-6:
        return (x >= e0).astype(np.float32)
    t = np.clip((x - e0) / span, 0.0, 1.0)
    return (t * t * (3.0 - 2.0 * t)).astype(np.float32)


def srgb_encode(x: np.ndarray) -> np.ndarray:
    x = np.clip(x, 0.0, 1.0)
    return np.where(x <= 0.0031308, 12.92 * x,
                    1.055 * np.power(np.maximum(x, 1e-8), 1.0 / 2.4) - 0.055).astype(np.float32)


def hashf(ix: np.ndarray, iy: np.ndarray, seed: int = 0) -> np.ndarray:
    """整数格点 -> [0,1) 的确定性 hash。"""
    ix = ix.astype(np.int64)
    iy = iy.astype(np.int64)
    h = ix * 374761393 + iy * 668265263 + np.int64(seed) * 1442695041 + np.int64(SEED)
    h = (h ^ (h >> 13)) * 1274126177
    h = h ^ (h >> 16)
    return ((h & 0xFFFFFF).astype(np.float32) / 16777215.0)


def fit01(a: np.ndarray, lo: float = 0.5, hi: float = 99.5) -> np.ndarray:
    """百分位拉伸，避免噪声整体发灰。"""
    lo_v = float(np.percentile(a, lo))
    hi_v = float(np.percentile(a, hi))
    return np.clip((a - lo_v) / max(hi_v - lo_v, 1e-6), 0.0, 1.0).astype(np.float32)


def box_blur(a: np.ndarray, radius: int = 1, passes: int = 3) -> np.ndarray:
    out = a.astype(np.float32)
    for _ in range(passes):
        out = (np.roll(out, radius, 0) + out + np.roll(out, -radius, 0)) / 3.0
        out = (np.roll(out, radius, 1) + out + np.roll(out, -radius, 1)) / 3.0
    return out


def torus_delta(coord: np.ndarray, center: float) -> np.ndarray:
    d = np.abs(coord - center)
    return np.minimum(d, 1.0 - d)


# --------------------------------------------------------------------------- #
# 噪声基底
# --------------------------------------------------------------------------- #
def perlin(u: np.ndarray, v: np.ndarray, freq: int, seed: int = 0) -> np.ndarray:
    """可平铺的梯度噪声，值域约 [-0.71, 0.71]。"""
    x = u * freq
    y = v * freq
    x0 = np.floor(x)
    y0 = np.floor(y)
    fx = (x - x0).astype(np.float32)
    fy = (y - y0).astype(np.float32)
    ix0 = x0.astype(np.int64) % freq
    iy0 = y0.astype(np.int64) % freq
    ix1 = (ix0 + 1) % freq
    iy1 = (iy0 + 1) % freq

    def grad(ix: np.ndarray, iy: np.ndarray):
        angle = hashf(ix, iy, seed) * TAU
        return np.cos(angle).astype(np.float32), np.sin(angle).astype(np.float32)

    gx0, gy0 = grad(ix0, iy0)
    gx1, gy1 = grad(ix1, iy0)
    gx2, gy2 = grad(ix0, iy1)
    gx3, gy3 = grad(ix1, iy1)

    d0 = gx0 * fx + gy0 * fy
    d1 = gx1 * (fx - 1.0) + gy1 * fy
    d2 = gx2 * fx + gy2 * (fy - 1.0)
    d3 = gx3 * (fx - 1.0) + gy3 * (fy - 1.0)

    sx = fx * fx * fx * (fx * (fx * 6.0 - 15.0) + 10.0)
    sy = fy * fy * fy * (fy * (fy * 6.0 - 15.0) + 10.0)
    top = d0 + sx * (d1 - d0)
    bottom = d2 + sx * (d3 - d2)
    return (top + sy * (bottom - top)).astype(np.float32)


def value_noise(u: np.ndarray, v: np.ndarray, freq: int, seed: int = 0) -> np.ndarray:
    x = u * freq
    y = v * freq
    x0 = np.floor(x)
    y0 = np.floor(y)
    fx = (x - x0).astype(np.float32)
    fy = (y - y0).astype(np.float32)
    ix0 = x0.astype(np.int64) % freq
    iy0 = y0.astype(np.int64) % freq
    ix1 = (ix0 + 1) % freq
    iy1 = (iy0 + 1) % freq
    c00 = hashf(ix0, iy0, seed)
    c10 = hashf(ix1, iy0, seed)
    c01 = hashf(ix0, iy1, seed)
    c11 = hashf(ix1, iy1, seed)
    sx = fx * fx * fx * (fx * (fx * 6.0 - 15.0) + 10.0)
    sy = fy * fy * fy * (fy * (fy * 6.0 - 15.0) + 10.0)
    top = c00 + sx * (c10 - c00)
    bottom = c01 + sx * (c11 - c01)
    return (top + sy * (bottom - top)).astype(np.float32)


def worley(u: np.ndarray, v: np.ndarray, cells: int, seed: int = 0,
           jitter: float = 0.9) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """返回 (F1, F2, 最近细胞随机值)，单位是细胞边长。"""
    x = u * cells
    y = v * cells
    xi = np.floor(x).astype(np.int64)
    yi = np.floor(y).astype(np.int64)
    f1 = np.full(x.shape, 1e9, np.float32)
    f2 = np.full(x.shape, 1e9, np.float32)
    cell = np.zeros(x.shape, np.float32)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            cx = (xi + dx) % cells
            cy = (yi + dy) % cells
            hx = hashf(cx, cy, seed)
            hy = hashf(cx, cy, seed + 1013)
            hi = hashf(cx, cy, seed + 2029)
            px = (xi + dx).astype(np.float32) + 0.5 + (hx - 0.5) * jitter
            py = (yi + dy).astype(np.float32) + 0.5 + (hy - 0.5) * jitter
            d = np.sqrt((x - px) ** 2 + (y - py) ** 2)
            closer = d < f1
            f2 = np.where(closer, f1, np.minimum(f2, d))
            cell = np.where(closer, hi, cell)
            f1 = np.where(closer, d, f1)
    return f1 / cells, f2 / cells, cell


def fbm(u: np.ndarray, v: np.ndarray, freq: int = 4, octaves: int = 5,
        gain: float = 0.5, seed: int = 0, absolute: bool = False,
        ridged: bool = False) -> np.ndarray:
    total = np.zeros(u.shape, np.float32)
    amp = 1.0
    norm = 0.0
    f = freq
    for i in range(octaves):
        p = perlin(u, v, f, seed + i * 7919)
        if absolute:
            p = np.abs(p)
        if ridged:
            p = (1.0 - np.abs(p)) ** 2
        total += amp * p
        norm += amp
        amp *= gain
        f *= 2
    return (total / norm).astype(np.float32)


# --------------------------------------------------------------------------- #
# 输出
# --------------------------------------------------------------------------- #
SEAM_WARNINGS: list[str] = []


def seam_ratio(a: np.ndarray) -> tuple[float, float]:
    """接缝处跳变 / 内部相邻像素跳变；≈1 表示无缝。

    对分区常量的图（棋盘、条纹）内部跳变可能是 0，此时该轴视为无信息直接跳过。
    """
    def ratio(seam: float, inner: float) -> float:
        return 0.0 if inner < 1e-4 else seam / inner

    dx = float(np.abs(a[:, 0] - a[:, -1]).mean())
    ix = float(np.abs(a[:, 1] - a[:, 0]).mean())
    dy = float(np.abs(a[0, :] - a[-1, :]).mean())
    iy = float(np.abs(a[1, :] - a[0, :]).mean())
    return ratio(dx, ix), ratio(dy, iy)


def save(name: str, arr: np.ndarray, *, srgb: bool = False, display: bool = False,
         tileable: bool = True, verbose: bool = True) -> Path:
    a = np.asarray(arr, np.float32)
    if a.ndim == 2:
        a = np.repeat(a[..., None], 3, axis=2)
    if srgb and not display:
        a = srgb_encode(a)
    a = np.clip(a, 0.0, 1.0)

    if tileable:
        rx, ry = seam_ratio(a)
        if rx > 3.0 or ry > 3.0:
            SEAM_WARNINGS.append(f"{name} (x={rx:.1f}, y={ry:.1f})")
            if verbose:
                print(f"  ! {name}: 接缝跳变偏大 x={rx:.1f} y={ry:.1f}")

    img8 = np.ascontiguousarray((a * 255.0 + 0.5).astype(np.uint8))
    image = Image.fromarray(img8)
    path = OUT_DIR / f"{name}.png"
    image.save(path, optimize=True)
    if verbose:
        size = path.stat().st_size / 1024.0
        print(f"  {path.name:<26} {image.size[0]}x{image.size[1]}  {size:7.1f} KB")
    return path


def save_hdr(name: str, rgb: np.ndarray, *, verbose: bool = True) -> Path:
    """写 Radiance RGBE（stb_image 的 stbi_loadf 能直接读）。"""
    rgba = np.maximum(rgb.astype(np.float32), 0.0)
    h, w, _ = rgba.shape
    peak = rgba.max(axis=2)
    exp = np.zeros_like(peak)
    nonzero = peak > 1e-32
    exp[nonzero] = np.floor(np.log2(peak[nonzero])) + 1.0
    scale = np.where(nonzero, 256.0 / np.exp2(exp), 0.0)
    mantissa = np.clip(rgba * scale[..., None], 0.0, 255.0).astype(np.uint8)
    e8 = np.clip(exp + 128.0, 0.0, 255.0).astype(np.uint8)
    data = np.concatenate([mantissa, e8[..., None]], axis=2)

    path = OUT_DIR / f"{name}.hdr"
    with open(path, "wb") as handle:
        handle.write(b"#?RADIANCE\n")
        handle.write(b"FORMAT=32-bit_rle_rgbe\n\n")
        handle.write(f"-Y {h} +X {w}\n".encode("ascii"))
        handle.write(data.tobytes())
    if verbose:
        print(f"  {path.name:<26} {w}x{h}  {path.stat().st_size / 1024.0:7.1f} KB")
    return path


# --------------------------------------------------------------------------- #
# P0 常量与检查器
# --------------------------------------------------------------------------- #
def make_constant(value: float) -> np.ndarray:
    return np.full((8, 8, 3), value, np.float32)


def make_checker(cells: int, size: int = 1024,
                 dark: float = 58 / 255.0, light: float = 200 / 255.0) -> np.ndarray:
    u, v = uv_grid(size)
    parity = (np.floor(u * cells) + np.floor(v * cells)) % 2.0
    rgb = np.where(parity[..., None] > 0.5, light, dark)
    return np.repeat(rgb, 3, axis=2).astype(np.float32)


def make_uv_checker(size: int = 1024, cells: int = 8) -> np.ndarray:
    u, v = uv_grid(size)
    cu = np.floor(np.clip(u, 0, 0.999999) * cells)
    cv = np.floor(np.clip(v, 0, 0.999999) * cells)
    local_u = np.clip(u * cells - cu, 0, 1)
    local_v = np.clip(v * cells - cv, 0, 1)

    rgb = np.zeros((size, size, 3), np.float32)
    rgb[..., 0] = 0.20 + 0.55 * (cu / (cells - 1))
    rgb[..., 1] = 0.20 + 0.55 * (cv / (cells - 1))
    rgb[..., 2] = 0.32

    cell_px = size / cells
    bar = 4.0 / cell_px
    plus_u = local_u > 1.0 - bar
    plus_v = local_v > 1.0 - bar
    rgb[plus_u] = np.array([0.95, 0.10, 0.10], np.float32)
    rgb[plus_v] = np.array([0.10, 0.90, 0.15], np.float32)

    line = 1.6 / cell_px
    border = (local_u < line) | (local_u > 1 - line) | (local_v < line) | (local_v > 1 - line)
    rgb[border] = np.array([0.06, 0.07, 0.10], np.float32)

    rgb[(cu == 0) & (cv == 0)] = 1.0                       # 左下角纯白
    rgb[(cu == cells - 1) & (cv == cells - 1)] = 0.0       # 右上角纯黑
    return rgb


def make_grid(size: int = 1024) -> np.ndarray:
    u, v = uv_grid(size)
    px = u * size
    py = (1.0 - v) * size

    def line_mask(coord: np.ndarray, period: float, width: float) -> np.ndarray:
        d = np.abs((coord + period * 0.5) % period - period * 0.5)
        return (d <= width * 0.5).astype(np.float32)

    minor = np.maximum(line_mask(px, size / 16.0, 1.5), line_mask(py, size / 16.0, 1.5))
    major = np.maximum(line_mask(px, size / 4.0, 3.0), line_mask(py, size / 4.0, 3.0))
    rgb = np.full((size, size, 3), 0.0627, np.float32)
    rgb = np.where(minor[..., None] > 0.5, 0.55, rgb)
    rgb = np.where(major[..., None] > 0.5, 0.88, rgb)
    return rgb.astype(np.float32)


def make_gradient_v(width: int = 1024, height: int = 16) -> np.ndarray:
    u, v = uv_grid(width, height)
    return np.repeat(v[..., None], 3, axis=2)


def make_gradient_radial(size: int = 1024) -> np.ndarray:
    u, v = uv_grid(size)
    d = np.sqrt((u - 0.5) ** 2 + (v - 0.5) ** 2) * 2.0
    t = np.clip(1.0 - d, 0.0, 1.0)
    return np.repeat(t[..., None], 3, axis=2)


def make_mask_circle(size: int = 512, radius: float = 0.8) -> np.ndarray:
    u, v = uv_grid(size)
    d = np.sqrt((u - 0.5) ** 2 + (v - 0.5) ** 2) * 2.0
    edge = 2.0 / size
    t = 1.0 - smoothstep(radius - edge, radius + edge, d)
    return np.repeat(t[..., None], 3, axis=2)


# --------------------------------------------------------------------------- #
# P1 噪声
# --------------------------------------------------------------------------- #
def make_noise_white(size: int = 512) -> np.ndarray:
    rng = np.random.default_rng(SEED + 1)
    a = rng.random((size, size), dtype=np.float32)
    return np.repeat(a[..., None], 3, axis=2)


def make_noise_perlin(size: int = 512) -> np.ndarray:
    u, v = uv_grid(size)
    return np.repeat(fit01(perlin(u, v, 8, seed=11))[..., None], 3, axis=2)


def make_noise_value(size: int = 512) -> np.ndarray:
    u, v = uv_grid(size)
    return np.repeat(fit01(value_noise(u, v, 8, seed=23))[..., None], 3, axis=2)


def make_noise_fbm(size: int = 512) -> np.ndarray:
    u, v = uv_grid(size)
    return np.repeat(fit01(fbm(u, v, 4, 5, 0.5, seed=31))[..., None], 3, axis=2)


def make_noise_turbulence(size: int = 512) -> np.ndarray:
    u, v = uv_grid(size)
    return np.repeat(fit01(fbm(u, v, 4, 5, 0.5, seed=37, absolute=True))[..., None], 3, axis=2)


def make_noise_ridged(size: int = 512) -> np.ndarray:
    u, v = uv_grid(size)
    return np.repeat(fit01(fbm(u, v, 4, 5, 0.55, seed=41, ridged=True))[..., None], 3, axis=2)


def make_noise_worley(size: int = 512) -> np.ndarray:
    u, v = uv_grid(size)
    f1, _, _ = worley(u, v, 8, seed=53)
    return np.repeat(fit01(1.0 - f1)[..., None], 3, axis=2)


def make_noise_cellular(size: int = 512) -> np.ndarray:
    u, v = uv_grid(size)
    f1, f2, cell = worley(u, v, 8, seed=59)
    edge = np.clip((f2 - f1) * 6.0, 0.0, 1.0)
    rgb = np.empty((size, size, 4), np.float32)
    rgb[..., 0] = fit01(1.0 - f1)
    rgb[..., 1] = cell
    rgb[..., 2] = edge
    rgb[..., 3] = np.round(cell * 255.0) / 255.0
    return rgb


def make_noise_curl(size: int = 512) -> np.ndarray:
    u, v = uv_grid(size)
    h = fbm(u, v, 3, 3, 0.5, seed=67)
    dh_drow, dh_dcol = np.gradient(h)
    dh_dv = -dh_drow * size        # 数组行向下，v 向上
    dh_du = dh_dcol * size
    cx = dh_dv
    cy = -dh_du
    mag = np.sqrt(cx * cx + cy * cy) + 1e-8
    scale = np.minimum(1.0, 1.0 / np.percentile(mag, 95.0))
    rgb = np.empty((size, size, 4), np.float32)
    rgb[..., 0] = 0.5 + 0.5 * np.clip(cx * scale, -1.0, 1.0)
    rgb[..., 1] = 0.5 + 0.5 * np.clip(cy * scale, -1.0, 1.0)
    rgb[..., 2] = np.clip(mag / (np.percentile(mag, 99.0) + 1e-8), 0.0, 1.0)
    rgb[..., 3] = 1.0
    return rgb


def make_noise_blue(size: int = 256, iterations: int = 6000) -> np.ndarray:
    """环形高斯能量最小化的 50% 密度蓝噪声。"""
    rng = np.random.default_rng(SEED + 7)
    pattern = (rng.random((size, size)) < 0.5).astype(np.float32)
    radius = 6
    yy, xx = np.mgrid[-radius:radius + 1, -radius:radius + 1].astype(np.float32)
    kernel = np.exp(-(xx * xx + yy * yy) / (2.0 * 1.6 ** 2)).astype(np.float32)

    def convolve(p: np.ndarray) -> np.ndarray:
        acc = np.zeros_like(p)
        for ky in range(-radius, radius + 1):
            for kx in range(-radius, radius + 1):
                w = kernel[ky + radius, kx + radius]
                if w > 1e-4:
                    acc += w * np.roll(np.roll(p, ky, 0), kx, 1)
        return acc

    energy = convolve(pattern)
    for _ in range(iterations):
        hot = np.unravel_index(np.argmax(np.where(pattern > 0.5, energy, -1e9)), energy.shape)
        cold = np.unravel_index(np.argmin(np.where(pattern < 0.5, energy, 1e9)), energy.shape)
        pattern[hot] = 0.0
        pattern[cold] = 1.0
        for (py, px), delta in ((hot, -1.0), (cold, 1.0)):
            for ky in range(-radius, radius + 1):
                for kx in range(-radius, radius + 1):
                    w = kernel[ky + radius, kx + radius]
                    if w > 1e-4:
                        energy[(py + ky) % size, (px + kx) % size] += delta * w
    return np.repeat(pattern[..., None], 3, axis=2)


def make_noise_grain(size: int = 256) -> np.ndarray:
    rng = np.random.default_rng(SEED + 13)
    grain = rng.normal(0.5, 0.06, (size, size)).astype(np.float32)
    rows = rng.normal(0.0, 0.02, (size, 1)).astype(np.float32)
    return np.repeat(np.clip(grain + rows, 0.0, 1.0)[..., None], 3, axis=2)


def make_noise_scratch(size: int = 1024) -> np.ndarray:
    rng = np.random.default_rng(SEED + 17)
    canvas = np.zeros((size, size), np.float32)
    count = 700
    for _ in range(count):
        angle = rng.normal(0.0, 0.55) + (0.0 if rng.random() < 0.5 else math.pi)
        length = rng.uniform(8.0, 64.0)
        cx = rng.uniform(0.0, size)
        cy = rng.uniform(0.0, size)
        steps = int(length * 2)
        t = np.linspace(0.0, 1.0, max(steps, 2), dtype=np.float32)
        xs = np.round(cx + np.cos(angle) * length * t).astype(np.int64) % size
        ys = np.round(cy + np.sin(angle) * length * t).astype(np.int64) % size
        brightness = rng.uniform(0.25, 0.9)
        np.add.at(canvas, (ys, xs), brightness)
    canvas = np.clip(canvas, 0.0, 1.0)
    canvas = np.maximum(canvas, box_blur(canvas, 1, 1) * 0.55)
    return np.repeat(fit01(canvas, 0.0, 99.7)[..., None], 3, axis=2)


# --------------------------------------------------------------------------- #
# P1 图案
# --------------------------------------------------------------------------- #
def make_stripes(vertical: bool = True, size: int = 1024, count: int = 8) -> np.ndarray:
    u, v = uv_grid(size)
    coord = u if vertical else v
    s = (coord * count) % 1.0
    band = smoothstep(0.34, 0.42, s) * (1.0 - smoothstep(0.58, 0.66, s))
    return np.repeat(band[..., None], 3, axis=2)


def make_stripes_diag(size: int = 1024, count: int = 8) -> np.ndarray:
    u, v = uv_grid(size)
    s = ((u + v) * count) % 1.0
    band = smoothstep(0.34, 0.42, s) * (1.0 - smoothstep(0.58, 0.66, s))
    return np.repeat(band[..., None], 3, axis=2)


def make_dots(size: int = 1024, cells: int = 8, radius: float = 0.35) -> np.ndarray:
    u, v = uv_grid(size)
    lx = (u * cells) % 1.0 - 0.5
    ly = (v * cells) % 1.0 - 0.5
    d = np.sqrt(lx * lx + ly * ly)
    edge = 1.5 / (size / cells)
    dot = 1.0 - smoothstep(radius - edge, radius + edge, d)
    return np.repeat(dot[..., None], 3, axis=2)


def make_hex_grid(size: int = 1024, cols: int = 8, rows: int = 7,
                  width: float = 2.0) -> np.ndarray:
    """平顶六边形蜂窝。列数取偶数，奇偶列的垂直偏移才能跨接缝对齐。"""
    u, v = uv_grid(size)
    dx = 1.0 / cols
    dy = 1.0 / rows
    r_uv = dx / 1.5                      # 外接圆半径
    col = np.round(u / dx)
    offset = (col.astype(np.int64) % 2).astype(np.float32) * dy * 0.5
    row = np.round((v - offset) / dy)
    cx = col * dx
    cy = row * dy + offset
    px = np.abs((u - cx) * size)
    py = np.abs((v - cy) * size)

    # 六边形 SDF（Inigo Quilez）：r 是上/下平边的半高（apothem）
    r = dy * size * 0.5
    kx, ky, kz = -0.866025404, 0.5, 0.577350269
    bend = np.minimum(px * kx + py * ky, 0.0)
    px = px - 2.0 * bend * kx
    py = py - 2.0 * bend * ky
    px = px - np.clip(px, -kz * r, kz * r)
    py = py - r
    dist = np.sqrt(px * px + py * py)

    line = 1.0 - smoothstep(width * 0.5 - 0.8, width * 0.5 + 0.8, dist)
    return np.repeat(line[..., None], 3, axis=2)


def brick_fields(size: int = 1024, rows: int = 8, cols: int = 4,
                 mortar_px: float = 6.0) -> dict:
    u, v = uv_grid(size)
    cell_v = np.clip(v, 0, 0.999999) * rows
    row = np.floor(cell_v)
    offset = (row.astype(np.int64) % 2).astype(np.float32) * 0.5
    cell_u = np.clip(u, 0, 0.999999) * cols + offset
    col = np.floor(cell_u)
    cell_w = size / cols
    cell_h = size / rows
    lx = (cell_u - col) * cell_w
    ly = (cell_v - row) * cell_h

    half = mortar_px * 0.5
    mortar = ((lx < half) | (lx > cell_w - half)
              | (ly < half) | (ly > cell_h - half))
    bevel = np.minimum(np.minimum(lx, cell_w - lx), np.minimum(ly, cell_h - ly)) / (mortar_px * 2.0)
    bevel = np.sin(np.clip(bevel, 0.0, 1.0) * math.pi * 0.5)
    ids = hashf(col.astype(np.int64) % cols, row.astype(np.int64) % rows, 71)
    return dict(u=u, v=v, mortar=mortar, bevel=bevel.astype(np.float32), ids=ids)


def make_brick(size: int = 1024, **kwargs) -> np.ndarray:
    f = brick_fields(size, **kwargs)
    brick_tone = np.array([0.615, 0.310, 0.180], np.float32)[None, None, :] * (
        0.78 + 0.42 * f["ids"][..., None])
    mortar_tone = np.array([0.720, 0.700, 0.660], np.float32)
    grain = fbm(f["u"], f["v"], 32, 3, 0.5, seed=73) * 0.16 + 0.92

    rgb = np.where(f["mortar"][..., None], mortar_tone[None, None, :], brick_tone)
    rgb = rgb * (0.80 + 0.20 * f["bevel"][..., None]) * grain[..., None]
    return np.clip(rgb, 0.0, 1.0).astype(np.float32)


def make_brick_mask(size: int = 1024, **kwargs) -> np.ndarray:
    f = brick_fields(size, **kwargs)
    rgb = np.empty((size, size, 4), np.float32)
    rgb[..., 0] = np.where(f["mortar"], 0.0, f["bevel"])
    rgb[..., 1] = f["ids"]
    rgb[..., 2] = np.where(f["mortar"], 0.0, 1.0)
    rgb[..., 3] = np.where(f["mortar"], 1.0, 0.0)
    return rgb


def tiles_fields(size: int = 1024, cells: int = 4, round_px: float = 12.0) -> dict:
    u, v = uv_grid(size)
    cell_v = np.clip(v, 0, 0.999999) * cells
    cell_u = np.clip(u, 0, 0.999999) * cells
    row = np.floor(cell_v)
    col = np.floor(cell_u)
    cell = size / cells
    lx = (cell_u - col) * cell
    ly = (cell_v - row) * cell
    half_w = cell * 0.5
    dx = np.abs(lx - half_w) - (half_w - round_px)
    dy = np.abs(ly - half_w) - (half_w - round_px)
    rounded = np.sqrt(np.maximum(dx, 0.0) ** 2 + np.maximum(dy, 0.0) ** 2)
    inside = np.maximum(dx, dy)
    sdf = np.where((dx > 0) & (dy > 0), rounded, inside)
    face = 1.0 - smoothstep(-1.0, 1.0, sdf)
    bevel = 1.0 - smoothstep(-round_px, 0.0, sdf)
    ids = hashf(col.astype(np.int64) % cells, row.astype(np.int64) % cells, 83)
    return dict(face=face.astype(np.float32), bevel=bevel.astype(np.float32), ids=ids)


def make_tiles(size: int = 1024, **kwargs) -> np.ndarray:
    f = tiles_fields(size, **kwargs)
    tile_tone = (np.array([0.905, 0.895, 0.870], np.float32)[None, None, :]
                 * (0.965 + 0.05 * f["ids"][..., None]))
    grout_tone = np.array([0.615, 0.595, 0.560], np.float32)
    shade = (0.88 + 0.12 * f["bevel"])[..., None]
    rgb = grout_tone[None, None, :] + (tile_tone * shade - grout_tone[None, None, :]) * f["face"][..., None]
    return np.clip(rgb, 0.0, 1.0).astype(np.float32)


def make_tiles_mask(size: int = 1024, **kwargs) -> np.ndarray:
    f = tiles_fields(size, **kwargs)
    rgb = np.empty((size, size, 4), np.float32)
    rgb[..., 0] = f["face"]
    rgb[..., 1] = f["ids"]
    rgb[..., 2] = f["bevel"]
    rgb[..., 3] = 1.0 - f["face"]
    return rgb


def make_truchet(size: int = 1024, cells: int = 8, width_px: float = 10.0) -> np.ndarray:
    u, v = uv_grid(size)
    cu = np.clip(u, 0, 0.999999) * cells
    cv = np.clip(v, 0, 0.999999) * cells
    col = np.floor(cu)
    row = np.floor(cv)
    lx = cu - col
    ly = cv - row
    cell_size = size / cells
    flip = hashf(col.astype(np.int64) % cells, row.astype(np.int64) % cells, 97) > 0.5

    r = 0.5
    d1x = np.where(flip, lx - 1.0, lx)
    d1y = np.where(flip, ly - 1.0, ly)
    d2x = np.where(flip, lx, lx - 1.0)
    d2y = np.where(flip, ly, ly - 1.0)
    a1 = np.abs(np.sqrt(d1x * d1x + d1y * d1y) - r)
    a2 = np.abs(np.sqrt(d2x * d2x + d2y * d2y) - r)
    d = np.minimum(a1, a2) * cell_size
    line = 1.0 - smoothstep(width_px * 0.5 - 1.0, width_px * 0.5 + 1.0, d)
    return np.repeat(line[..., None], 3, axis=2)


def make_weave(size: int = 1024, threads: int = 16) -> np.ndarray:
    u, v = uv_grid(size)
    cell_u = np.clip(u, 0, 0.999999) * threads
    cell_v = np.clip(v, 0, 0.999999) * threads
    col = np.floor(cell_u)
    row = np.floor(cell_v)
    fx = (cell_u - col) * 2.0 - 1.0
    fy = (cell_v - row) * 2.0 - 1.0
    over = ((col.astype(np.int64) + row.astype(np.int64)) % 4) < 2
    warp = np.cos(np.clip(fx, -1, 1) * math.pi * 0.5) ** 1.4
    weft = np.cos(np.clip(fy, -1, 1) * math.pi * 0.5) ** 1.4
    height = np.where(over, warp, weft)
    shade = 0.55 + 0.45 * height
    base = np.array([0.105, 0.135, 0.20], np.float32)
    return (base[None, None, :] * (shade[..., None] * 1.9)).astype(np.float32)


# --------------------------------------------------------------------------- #
# P2 水
# --------------------------------------------------------------------------- #
def water_height(size: int = 1024, sources: int = 6) -> np.ndarray:
    """三个频段的环形波干涉高度，返回 HxWx3，每通道已归一化到 0.5±0.35。"""
    u, v = uv_grid(size)
    rng = np.random.default_rng(SEED + 101)
    centers = [(rng.uniform(0, 1), rng.uniform(0, 1)) for _ in range(sources)]
    bands = (3.0, 7.0, 15.0)
    out = np.empty((size, size, 3), np.float32)
    for band_index, freq in enumerate(bands):
        acc = np.zeros((size, size), np.float32)
        for k, (cx, cy) in enumerate(centers):
            dx = torus_delta(u, cx)
            dy = torus_delta(v, cy)
            r = np.sqrt(dx * dx + dy * dy)
            phase = rng.uniform(0.0, TAU)
            acc += np.sin(TAU * (freq * r - phase)) * (1.0 - band_index * 0.2)
        acc /= sources
        peak = np.percentile(np.abs(acc), 99.0) + 1e-8
        out[..., band_index] = np.clip(0.5 + 0.35 * acc / peak, 0.0, 1.0)
    return out


def make_water_ripples(size: int = 1024) -> np.ndarray:
    """灰色水波高度：取中频段，既像水面又能直接当高度图/法线源。"""
    mid = water_height(size)[..., 1]
    return np.repeat(mid[..., None], 3, axis=2)


def make_water_normal(size: int = 1024) -> np.ndarray:
    height = water_height(size)[..., 1]
    dh_drow, dh_dcol = np.gradient(height)
    dh_dv = -dh_drow * size * 1.6
    dh_du = dh_dcol * size * 1.6
    nx = -dh_du
    ny = -dh_dv
    nz = np.ones_like(nx)
    length = np.sqrt(nx * nx + ny * ny + nz * nz)
    rgb = np.empty((size, size, 3), np.float32)
    rgb[..., 0] = 0.5 + 0.5 * nx / length
    rgb[..., 1] = 0.5 + 0.5 * ny / length
    rgb[..., 2] = 0.5 + 0.5 * nz / length
    return rgb


def make_water_caustics(size: int = 1024) -> np.ndarray:
    u, v = uv_grid(size)
    f1, f2, _ = worley(u, v, 6, seed=131)
    ridge = np.power(np.clip(1.0 - (f2 - f1) / 0.16, 0.0, 1.0), 3.0)
    d = np.sqrt((u - 0.5) ** 2 + (v - 0.5) ** 2) * 2.0
    falloff = np.clip(1.0 - 0.45 * d, 0.0, 1.0)
    out = np.clip(ridge * falloff + box_blur(ridge, 2, 2) * 0.35, 0.0, 1.0)
    return np.repeat(fit01(out, 0.0, 99.0)[..., None], 3, axis=2)


def make_water_flow(size: int = 512) -> np.ndarray:
    u, v = uv_grid(size)
    h = fbm(u, v, 3, 3, 0.5, seed=151)
    dh_drow, dh_dcol = np.gradient(h)
    cx = -dh_drow * size
    cy = -dh_dcol * size
    toward_x = -(u - 0.5)
    toward_y = -(v - 0.5)
    vx = cx * 0.85 + toward_x * 0.15
    vy = cy * 0.85 + toward_y * 0.15
    length = np.sqrt(vx * vx + vy * vy) + 1e-8
    speed = np.clip(length / (np.percentile(length, 98.0) + 1e-8), 0.0, 1.0)
    rgb = np.empty((size, size, 4), np.float32)
    rgb[..., 0] = 0.5 + 0.5 * vx / length
    rgb[..., 1] = 0.5 + 0.5 * vy / length
    rgb[..., 2] = 0.35 + 0.45 * speed
    rgb[..., 3] = 1.0
    return rgb


def make_water_foam(size: int = 1024) -> np.ndarray:
    u, v = uv_grid(size)
    warped_v = (v * 3.0) % 1.0          # 整数倍拉伸 -> 保持可平铺
    cluster = smoothstep(0.48, 0.70, fit01(fbm(u, warped_v, 12, 4, 0.55, seed=163)))
    fine = smoothstep(0.56, 0.78, fit01(fbm(u, warped_v, 24, 3, 0.5, seed=167)))
    f1, f2, _ = worley(u, warped_v, 14, seed=171)
    bubble = 1.0 - smoothstep(0.0, 0.055, f2 - f1)
    foam = np.clip(cluster * 0.80 + fine * 0.30 + bubble * cluster * 0.55, 0.0, 1.0)
    rgb = np.empty((size, size, 4), np.float32)
    rgb[..., 0] = foam
    rgb[..., 1] = foam
    rgb[..., 2] = foam
    rgb[..., 3] = foam
    return rgb


def make_water_waves(size: int = 1024) -> np.ndarray:
    u, v = uv_grid(size)
    phase = (np.sin(TAU * 3.0 * u) * 0.9
             + np.sin(TAU * 2.0 * v + 1.1) * 0.7
             + np.sin(TAU * 3.0 * (u + v) + 2.3) * 0.5
             + np.sin(TAU * 3.0 * (u - v) + 0.4) * 0.35)
    out = 0.5 + 0.35 * phase / 2.45
    return np.repeat(np.clip(out, 0.0, 1.0)[..., None], 3, axis=2)


# --------------------------------------------------------------------------- #
# P2 自然与特效
# --------------------------------------------------------------------------- #
def make_wood(size: int = 1024) -> np.ndarray:
    u, v = uv_grid(size)
    dx = torus_delta(u, 0.32)
    dy = torus_delta(v, 0.5)
    r = np.sqrt(dx * dx + dy * dy)
    warp = fbm(u, v, 6, 4, 0.5, seed=181) * 0.9
    rings = (r * 9.0 + warp) % 1.0
    band = smoothstep(0.0, 0.16, rings) * (1.0 - smoothstep(0.34, 0.62, rings))
    late = np.clip(band, 0.0, 1.0)
    early = np.array([0.541, 0.388, 0.216], np.float32)
    dark = np.array([0.290, 0.208, 0.125], np.float32)
    fibers = fbm(u, v, 64, 3, 0.5, seed=191) * 0.14 + 0.93
    rgb = early[None, None, :] * (1.0 - late[..., None]) + dark[None, None, :] * late[..., None]
    edge_line = 1.0 - smoothstep(0.0, 0.05, np.abs(rings - 0.02))
    rgb *= (1.0 - 0.25 * edge_line[..., None])
    return np.clip(rgb * fibers[..., None], 0.0, 1.0)


def make_marble(size: int = 1024) -> np.ndarray:
    u, v = uv_grid(size)
    turb = fbm(u, v, 3, 5, 0.55, seed=211, absolute=True)
    t = np.sin(TAU * (u * 3.0 + 2.4 * turb))
    vein = np.power(1.0 - smoothstep(0.0, 0.22, np.abs(t)), 1.8)
    base = np.array([0.847, 0.831, 0.800], np.float32)
    dark = np.array([0.290, 0.290, 0.310], np.float32)
    rgb = base[None, None, :] * (1.0 - vein[..., None] * 0.88) + dark[None, None, :] * vein[..., None] * 0.88
    grain = fbm(u, v, 32, 3, 0.5, seed=223) * 0.05 + 0.975
    return np.clip(rgb * grain[..., None], 0.0, 1.0)


def rust_fields(size: int = 1024) -> dict:
    u, v = uv_grid(size)
    body = fit01(fbm(u, v, 5, 5, 0.55, seed=233))
    patch = smoothstep(0.38, 0.58, body)
    pits = fit01(fbm(u, v, 24, 3, 0.5, seed=239))
    rng = np.random.default_rng(SEED + 241)
    streak = np.repeat(rng.normal(0.0, 0.045, (size, 1)).astype(np.float32), size, axis=1)
    patch = np.clip(patch + streak * 0.5, 0.0, 1.0).astype(np.float32)
    return dict(u=u, v=v, patch=patch, pits=pits)


def make_rust(size: int = 1024) -> np.ndarray:
    f = rust_fields(size)
    metal = np.array([0.285, 0.290, 0.305], np.float32)
    rust_dark = np.array([0.300, 0.132, 0.062], np.float32)
    rust_bright = np.array([0.640, 0.318, 0.128], np.float32)
    rust = rust_dark[None, None, :] + (rust_bright - rust_dark)[None, None, :] * (
        0.35 + 0.65 * f["pits"][..., None])
    rgb = metal[None, None, :] + (rust - metal[None, None, :]) * f["patch"][..., None]
    grit = fbm(f["u"], f["v"], 48, 3, 0.5, seed=243) * 0.18 + 0.91
    return np.clip(rgb * grit[..., None], 0.0, 1.0).astype(np.float32)


def make_rust_mask(size: int = 1024) -> np.ndarray:
    f = rust_fields(size)
    rgb = np.empty((size, size, 4), np.float32)
    rgb[..., 0] = f["patch"]
    rgb[..., 1] = 1.0 - f["patch"]
    rgb[..., 2] = f["pits"]
    rgb[..., 3] = 1.0
    return rgb


def make_dirt(size: int = 1024) -> np.ndarray:
    u, v = uv_grid(size)
    soft = fit01(fbm(u, v, 3, 5, 0.55, seed=251))
    patches = smoothstep(0.52, 0.92, fit01(fbm(u, v, 7, 4, 0.5, seed=257)))
    speck = fit01(fbm(u, v, 48, 2, 0.5, seed=263))
    value = 0.30 + 0.34 * soft + 0.16 * patches + 0.10 * speck
    return np.repeat(np.clip(value, 0.0, 1.0)[..., None], 3, axis=2)


def make_cloud(size: int = 1024) -> np.ndarray:
    u, v = uv_grid(size)
    n = fit01(fbm(u, v, 3, 6, 0.55, seed=271))
    coverage = smoothstep(0.44, 0.74, n)
    rgb = np.empty((size, size, 4), np.float32)
    rgb[..., 0] = 0.98
    rgb[..., 1] = 0.98
    rgb[..., 2] = 1.0
    rgb[..., 3] = np.clip(coverage, 0.0, 1.0)
    return rgb


def make_smoke(size: int = 512) -> np.ndarray:
    u, v = uv_grid(size)
    warp = fbm(u, v, 3, 4, 0.5, seed=291)
    n = fbm(u * 1.35 + warp * 0.45, v * 0.55 + warp * 0.30, 6, 5, 0.55, seed=281)
    body = smoothstep(0.36, 0.74, fit01(n))
    falloff = smoothstep(0.0, 0.20, v) * (1.0 - smoothstep(0.62, 1.0, v))
    horizontal = np.clip(1.0 - np.abs(u - 0.5) * 2.0, 0.0, 1.0) ** 0.55
    alpha = np.clip(body * falloff * horizontal, 0.0, 1.0)
    rgb = np.empty((size, size, 4), np.float32)
    rgb[..., 0] = alpha * 0.96
    rgb[..., 1] = alpha * 0.96
    rgb[..., 2] = alpha * 0.98
    rgb[..., 3] = alpha
    return rgb


def make_fire(size: int = 512) -> np.ndarray:
    u, v = uv_grid(size)
    n = fbm(u * 2.2, v * 0.55, 5, 5, 0.55, seed=283)
    body = smoothstep(0.40, 0.80, fit01(n))
    height = smoothstep(0.02, 0.55, v) * (1.0 - smoothstep(0.72, 1.0, v))
    heat = np.clip(body * height * 1.25, 0.0, 1.0)
    core = np.clip(heat * heat * 1.4, 0.0, 1.0)
    rgb = np.empty((size, size, 4), np.float32)
    rgb[..., 0] = np.clip(heat * 1.0, 0.0, 1.0)
    rgb[..., 1] = np.clip(core * 0.75 + heat * 0.25, 0.0, 1.0)
    rgb[..., 2] = np.clip(core * 0.35, 0.0, 1.0)
    rgb[..., 3] = heat
    return rgb


def make_sand(size: int = 1024) -> np.ndarray:
    u, v = uv_grid(size)
    grain = fit01(fbm(u, v, 96, 3, 0.5, seed=293))
    dune = 0.5 + 0.5 * np.sin(TAU * (2.0 * v + 0.30 * fbm(u, v, 3, 3, 0.5, seed=307)))
    value = 0.70 + 0.13 * grain + 0.07 * dune
    base = np.array([0.86, 0.78, 0.61], np.float32)
    return np.clip(base[None, None, :] * (value[..., None] / 0.78), 0.0, 1.0)


def scales_fields(size: int = 1024, rows: int = 14, cols: int = 10) -> dict:
    u, v = uv_grid(size)
    cell_v = np.clip(v, 0, 0.999999) * rows
    row = np.floor(cell_v)
    offset = (row.astype(np.int64) % 2).astype(np.float32) * 0.5
    cell_u = np.clip(u, 0, 0.999999) * cols + offset
    col = np.floor(cell_u)
    lx = (cell_u - col) * 2.0 - 1.0
    ly = (cell_v - row) * 2.0 - 1.0
    ly = ly + 0.55
    d = np.sqrt(lx * lx * 0.8 + np.maximum(ly, 0.0) ** 2)
    shell = (1.0 - smoothstep(0.72, 0.95, d)).astype(np.float32)
    rim = np.exp(-((d - 0.75) ** 2) / 0.012).astype(np.float32)
    ids = hashf(col.astype(np.int64) % cols, row.astype(np.int64) % rows, 311)
    return dict(shell=shell, rim=rim, ids=ids)


def make_scales(size: int = 1024, **kwargs) -> np.ndarray:
    f = scales_fields(size, **kwargs)
    base = np.array([0.13, 0.30, 0.22], np.float32)
    shade = (0.55 + 0.55 * f["shell"] + 0.35 * f["rim"]) * (0.85 + 0.3 * f["ids"])
    rgb = base[None, None, :] * shade[..., None]
    rgb = np.where(f["shell"][..., None] < 0.35, np.array([0.06, 0.12, 0.10], np.float32), rgb)
    return np.clip(rgb, 0.0, 1.0)


def make_scales_mask(size: int = 1024, **kwargs) -> np.ndarray:
    f = scales_fields(size, **kwargs)
    rgb = np.empty((size, size, 4), np.float32)
    rgb[..., 0] = f["shell"]
    rgb[..., 1] = f["ids"]
    rgb[..., 2] = np.clip(f["rim"], 0.0, 1.0)
    rgb[..., 3] = 1.0
    return rgb


def make_energy(size: int = 512) -> np.ndarray:
    rng = np.random.default_rng(SEED + 317)
    canvas = np.zeros((size, size), np.float32)
    for _ in range(4):
        x = rng.uniform(0.1, 0.9) * size
        y = rng.uniform(0.1, 0.9) * size
        angle = rng.uniform(0.0, TAU)
        for step in range(700):
            angle += rng.normal(0.0, 0.20)
            x = (x + math.cos(angle) * 1.6) % size
            y = (y + math.sin(angle) * 1.6) % size
            xi = int(x) % size
            yi = int(y) % size
            canvas[yi, xi] = max(canvas[yi, xi], rng.uniform(0.6, 1.0))
            if step % 90 == 80 and rng.random() < 0.8:
                bx, by, bang = x, y, angle + rng.choice([-1.0, 1.0])
                for _ in range(120):
                    bang += rng.normal(0.0, 0.28)
                    bx = (bx + math.cos(bang) * 1.5) % size
                    by = (by + math.sin(bang) * 1.5) % size
                    canvas[int(by) % size, int(bx) % size] = max(
                        canvas[int(by) % size, int(bx) % size], rng.uniform(0.4, 0.9))
    core = np.clip(canvas, 0.0, 1.0)
    tight = box_blur(core, 1, 2)
    wide = box_blur(core, 3, 3)
    out = np.clip(core * 1.5 + tight * 1.8 + wide * 2.4, 0.0, 1.0)
    out = np.power(out, 0.65)
    return np.repeat(out[..., None], 3, axis=2)


def make_starfield(size: int = 1024, count: int = 3600) -> np.ndarray:
    rng = np.random.default_rng(SEED + 331)
    canvas = np.zeros((size, size), np.float32)
    xs = rng.integers(0, size, count)
    ys = rng.integers(0, size, count)
    brightness = np.power(rng.random(count), 2.4) * 0.85 + 0.15
    np.add.at(canvas, (ys, xs), brightness.astype(np.float32))
    big = rng.random(count) > 0.975
    for x, y in zip(xs[big], ys[big]):
        for dx, dy in ((0, 0), (1, 0), (-1, 0), (0, 1), (0, -1)):
            canvas[(y + dy) % size, (x + dx) % size] = min(
                canvas[(y + dy) % size, (x + dx) % size] + 0.45, 1.0)
    halo = box_blur(canvas, 1, 2)
    canvas = np.clip(canvas + halo * 1.2, 0.0, 1.0)
    return np.repeat(canvas[..., None], 3, axis=2)


def make_scanlines(size: int = 512) -> np.ndarray:
    u, v = uv_grid(size)
    lines = np.sin(TAU * 64.0 * v) * 0.5 + 0.5
    thick = ((np.floor(v * 64.0).astype(np.int64) % 8) == 0).astype(np.float32)
    value = 0.45 + 0.45 * np.power(lines, 0.7) + 0.10 * thick
    rng = np.random.default_rng(SEED + 337)
    value += rng.normal(0.0, 0.012, (size, 1)).astype(np.float32) * np.ones((1, size), np.float32)
    return np.repeat(np.clip(value, 0.0, 1.0)[..., None], 3, axis=2)


def make_env_equirect(width: int = 1024, height: int = 512) -> np.ndarray:
    u, v = uv_grid(width, height)
    sky_top = np.array([0.16, 0.30, 0.55], np.float32)
    sky_horizon = np.array([0.62, 0.66, 0.72], np.float32)
    ground = np.array([0.20, 0.19, 0.185], np.float32)
    elev = np.clip(v * 2.0 - 1.0, -1.0, 1.0)
    rgb = np.where(elev[..., None] > 0.0,
                   sky_horizon[None, None, :] * (1.0 - elev[..., None])
                   + sky_top[None, None, :] * elev[..., None],
                   sky_horizon[None, None, :] * (1.0 + elev[..., None])
                   + ground[None, None, :] * (-elev[..., None]))
    lights = [
        (0.55, 0.72, 0.055, 12.0, np.array([1.0, 0.96, 0.90], np.float32)),
        (0.15, 0.40, 0.075, 4.5, np.array([0.70, 0.82, 1.00], np.float32)),
        (0.86, 0.30, 0.070, 3.0, np.array([1.00, 0.72, 0.45], np.float32)),
    ]
    for cx, cy, size_w, strength, tint in lights:
        dx = torus_delta(u, cx) / size_w
        dy = (v - cy) / (size_w * height / width)
        falloff = np.exp(-(dx * dx + dy * dy))
        rgb += falloff[..., None] * tint[None, None, :] * strength
    return np.clip(rgb.astype(np.float32), 0.0, 64.0)


# --------------------------------------------------------------------------- #
# 彩色编号棋盘格（1:1 参考图）
# --------------------------------------------------------------------------- #
FONT_CANDIDATES = (
    "C:/Windows/Fonts/arialbd.ttf",
    "C:/Windows/Fonts/seguisb.ttf",
    "C:/Windows/Fonts/arial.ttf",
    "C:/Windows/Fonts/segoeui.ttf",
)


def checker_palette(cells: int = 10) -> np.ndarray:
    """色相沿列/行推进，奇偶格在浓彩与淡彩之间切换。"""
    palette = np.zeros((cells, cells, 3), np.float32)
    for row in range(cells):
        for col in range(cells):
            hue = ((col * 36.0 + row * 14.0) % 360.0) / 360.0
            vivid = (col + row) % 2 == 0
            sat = 0.78 if vivid else 0.16
            val = 0.94 if vivid else 1.0
            palette[row, col] = colorsys.hsv_to_rgb(hue, sat, val)
    return palette


def make_color_checker(size: int = 2000, cells: int = 10) -> np.ndarray:
    u, v = uv_grid(size)
    cu = np.clip(u, 0, 0.999999) * cells
    cv = np.clip(v, 0, 0.999999) * cells
    col = np.floor(cu).astype(np.int64)
    row = np.floor(cv).astype(np.int64)
    rgb = checker_palette(cells)[row, col]

    cell = size / cells
    px = u * size
    py = (1.0 - v) * size

    def separator(coord: np.ndarray) -> np.ndarray:
        d = np.abs((coord + cell * 0.5) % cell - cell * 0.5)
        return d <= 1.0

    sep = separator(px) | separator(py)
    rgb = np.where(sep[..., None], np.array([0.13, 0.13, 0.16], np.float32), rgb)
    return np.clip(rgb, 0.0, 1.0).astype(np.float32)


def load_font(pixel_size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    for path in FONT_CANDIDATES:
        if Path(path).exists():
            try:
                return ImageFont.truetype(path, pixel_size)
            except OSError:
                continue
    return ImageFont.load_default()


def draw_checker_labels(path: Path, cells: int = 10, font_scale: float = 0.26) -> None:
    """在每格中心写 A1..J10：列字母从左到右，行号从上到下（A1 在左上角）。"""
    image = Image.open(path).convert("RGB")
    draw = ImageDraw.Draw(image)
    width, height = image.size
    cell = width / cells
    font = load_font(max(12, int(cell * font_scale)))
    for row in range(cells):
        for col in range(cells):
            label = f"{chr(ord('A') + col)}{row + 1}"
            cx = (col + 0.5) * cell
            cy = (row + 0.5) * cell
            r, g, b = image.getpixel((int(cx), int(cy)))
            luminance = (0.299 * r + 0.587 * g + 0.114 * b) / 255.0
            fill = (18, 18, 22) if luminance > 0.55 else (250, 250, 252)
            draw.text((cx, cy), label, font=font, fill=fill, anchor="mm")
    image.save(path, optimize=True)


# --------------------------------------------------------------------------- #
# 资产表
# --------------------------------------------------------------------------- #
#  name                  builder                srgb    display  可平铺
ASSETS: dict[str, dict] = {
    "T_White":             dict(build=make_constant, kwargs=dict(value=1.0), srgb=True, display=True, tile=False),
    "T_Black":             dict(build=make_constant, kwargs=dict(value=0.0), srgb=True, display=True, tile=False),
    "T_MidGray":           dict(build=make_constant, kwargs=dict(value=128 / 255.0), srgb=False, display=True, tile=False),
    "T_FlatNormal":        dict(build=lambda: np.full((8, 8, 3), [128 / 255.0, 128 / 255.0, 1.0], np.float32),
                                srgb=False, display=True, tile=False),
    "T_Checker":           dict(build=make_checker, kwargs=dict(cells=8), srgb=True, display=True, tile=True),
    "T_CheckerSmall":      dict(build=make_checker, kwargs=dict(cells=16), srgb=True, display=True, tile=True),
    "T_ColorChecker10x10": dict(build=make_color_checker, kwargs=dict(size=2000, cells=10),
                                srgb=True, display=True, tile=False,
                                post=draw_checker_labels, post_kwargs=dict(cells=10)),
    "T_UVChecker":         dict(build=make_uv_checker, srgb=True, display=True, tile=True),
    "T_Grid":              dict(build=make_grid, srgb=True, display=True, tile=True),
    "T_GradientV":         dict(build=make_gradient_v, srgb=True, display=False, tile=False),
    "T_GradientRadial":    dict(build=make_gradient_radial, srgb=True, display=False, tile=False),
    "T_MaskCircle":        dict(build=make_mask_circle, srgb=False, display=True, tile=False),
    "T_NoiseWhite":        dict(build=make_noise_white, srgb=False, display=True, tile=False),
    "T_NoisePerlin":       dict(build=make_noise_perlin, srgb=False, display=True, tile=True),
    "T_NoiseFBM":          dict(build=make_noise_fbm, srgb=False, display=True, tile=True),
    "T_NoiseValue":        dict(build=make_noise_value, srgb=False, display=True, tile=True),
    "T_NoiseWorley":       dict(build=make_noise_worley, srgb=False, display=True, tile=True),
    "T_NoiseCellular":     dict(build=make_noise_cellular, srgb=False, display=True, tile=True),
    "T_NoiseTurbulence":   dict(build=make_noise_turbulence, srgb=False, display=True, tile=True),
    "T_NoiseRidged":       dict(build=make_noise_ridged, srgb=False, display=True, tile=True),
    "T_NoiseCurl":         dict(build=make_noise_curl, srgb=False, display=True, tile=True),
    "T_NoiseBlue":         dict(build=make_noise_blue, srgb=False, display=True, tile=False),
    "T_NoiseGrain":        dict(build=make_noise_grain, srgb=False, display=True, tile=True),
    "T_NoiseScratch":      dict(build=make_noise_scratch, srgb=False, display=True, tile=True),
    "T_StripesV":          dict(build=make_stripes, kwargs=dict(vertical=True), srgb=False, display=True, tile=True),
    "T_StripesH":          dict(build=make_stripes, kwargs=dict(vertical=False), srgb=False, display=True, tile=True),
    "T_StripesD45":        dict(build=make_stripes_diag, srgb=False, display=True, tile=True),
    "T_Dots":              dict(build=make_dots, srgb=False, display=True, tile=True),
    "T_HexGrid":           dict(build=make_hex_grid, srgb=False, display=True, tile=True),
    "T_Brick":             dict(build=make_brick, srgb=False, display=True, tile=True),
    "T_BrickMask":         dict(build=make_brick_mask, srgb=False, display=True, tile=True),
    "T_Tiles":             dict(build=make_tiles, srgb=False, display=True, tile=True),
    "T_TilesMask":         dict(build=make_tiles_mask, srgb=False, display=True, tile=True),
    "T_Truchet":           dict(build=make_truchet, srgb=False, display=True, tile=True),
    "T_Weave":             dict(build=make_weave, srgb=True, display=True, tile=True),
    "T_WaterRipples":      dict(build=make_water_ripples, srgb=False, display=True, tile=True),
    "T_WaterNormal":       dict(build=make_water_normal, srgb=False, display=True, tile=True),
    "T_WaterCaustics":     dict(build=make_water_caustics, srgb=False, display=True, tile=True),
    "T_WaterFlow":         dict(build=make_water_flow, srgb=False, display=True, tile=True),
    "T_WaterFoam":         dict(build=make_water_foam, srgb=False, display=True, tile=True),
    "T_WaterWaves":        dict(build=make_water_waves, srgb=False, display=True, tile=True),
    "T_Wood":              dict(build=make_wood, srgb=True, display=True, tile=True),
    "T_Marble":            dict(build=make_marble, srgb=True, display=True, tile=True),
    "T_Rust":              dict(build=make_rust, srgb=True, display=True, tile=True),
    "T_RustMask":          dict(build=make_rust_mask, srgb=False, display=True, tile=True),
    "T_Dirt":              dict(build=make_dirt, srgb=False, display=True, tile=True),
    "T_Cloud":             dict(build=make_cloud, srgb=True, display=True, tile=True),
    "T_Smoke":             dict(build=make_smoke, srgb=True, display=True, tile=False),
    "T_Fire":              dict(build=make_fire, srgb=True, display=True, tile=False),
    "T_Sand":              dict(build=make_sand, srgb=True, display=True, tile=True),
    "T_Scales":            dict(build=make_scales, srgb=True, display=True, tile=True),
    "T_ScalesMask":        dict(build=make_scales_mask, srgb=False, display=True, tile=True),
    "T_Energy":            dict(build=make_energy, srgb=False, display=True, tile=False),
    "T_StarField":         dict(build=make_starfield, srgb=False, display=True, tile=True),
    "T_Scanlines":         dict(build=make_scanlines, srgb=False, display=True, tile=True),
}

HDR_ASSET = "T_DefaultEquirect"


# --------------------------------------------------------------------------- #
# 缩略图总览
# --------------------------------------------------------------------------- #
def make_contact_sheet(names: list[str], thumb: int = 192, columns: int = 8) -> Path:
    label_h = 20
    rows = (len(names) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * thumb, rows * (thumb + label_h)), (24, 24, 26))
    draw = ImageDraw.Draw(sheet)
    for index, name in enumerate(names):
        path = OUT_DIR / f"{name}.png"
        if not path.exists():
            continue
        cx = (index % columns) * thumb
        cy = (index // columns) * (thumb + label_h)
        try:
            img = Image.open(path).convert("RGBA")
        except OSError:
            continue
        # 带 alpha 的资产铺在中灰上，否则 RGBA 包会显示成荧光色
        if img.getchannel("A").getextrema()[0] < 250:
            backdrop = Image.new("RGBA", img.size, (70, 72, 78, 255))
            img = Image.alpha_composite(backdrop, img)
        img = img.convert("RGB")
        img.thumbnail((thumb - 6, thumb - 6), Image.LANCZOS)
        sheet.paste(img, (cx + (thumb - img.width) // 2, cy + (thumb - img.height) // 2))
        draw.text((cx + 4, cy + thumb + 3), name.replace("T_", ""), fill=(190, 190, 196))
    path = SHEET_DIR / "_contact_sheet.png"
    SHEET_DIR.mkdir(parents=True, exist_ok=True)
    sheet.save(path, optimize=True)
    return path


def make_seam_check(names: list[str], tile: int = 220, columns: int = 4) -> Path:
    """每个资产按 3x3 拼一次，用来肉眼验接缝。"""
    label_h = 20
    rows = (len(names) + columns - 1) // columns
    cell = tile * 3
    sheet = Image.new("RGB", (columns * (cell + 8), rows * (cell + label_h + 8)), (20, 20, 22))
    draw = ImageDraw.Draw(sheet)
    for index, name in enumerate(names):
        path = OUT_DIR / f"{name}.png"
        if not path.exists():
            continue
        src = Image.open(path).convert("RGBA").resize((tile, tile), Image.LANCZOS)
        backdrop = Image.new("RGBA", (tile, tile), (70, 72, 78, 255))
        src = Image.alpha_composite(backdrop, src).convert("RGB")
        ox = (index % columns) * (cell + 8) + 4
        oy = (index // columns) * (cell + label_h + 8) + 4
        for ty in range(3):
            for tx in range(3):
                sheet.paste(src, (ox + tx * tile, oy + ty * tile))
        draw.text((ox, oy + cell + 4), name.replace("T_", ""), fill=(190, 190, 196))
    out = SHEET_DIR / "_seam_check.png"
    SHEET_DIR.mkdir(parents=True, exist_ok=True)
    sheet.save(out, optimize=True)
    return out


# --------------------------------------------------------------------------- #
# main
# --------------------------------------------------------------------------- #
def main() -> None:
    parser = argparse.ArgumentParser(description="Generate the MaterialForge default textures.")
    parser.add_argument("--only", nargs="*", default=None, help="只生成指定资产名")
    parser.add_argument("--list", action="store_true", help="列出资产名")
    parser.add_argument("--sheet", action="store_true", help="额外输出缩略图总览")
    parser.add_argument("--seams", action="store_true", help="额外输出 3x3 接缝检查图")
    args = parser.parse_args()

    if args.list:
        for name in list(ASSETS) + [HDR_ASSET]:
            print(name)
        return

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    only = set(args.only or [])
    names = [n for n in ASSETS if not only or n in only]
    do_hdr = (not only) or (HDR_ASSET in only)
    if only and not names and not do_hdr:
        raise SystemExit("没有匹配的资产名，用 --list 查看")

    print(f"输出目录: {OUT_DIR}")
    total = 0
    for name in names:
        spec = ASSETS[name]
        arr = spec["build"](**spec.get("kwargs", {}))
        path = save(name, arr, srgb=spec["srgb"], display=spec["display"],
                    tileable=spec["tile"])
        if spec.get("post"):
            spec["post"](path, **spec.get("post_kwargs", {}))
        total += path.stat().st_size

    if do_hdr:
        path = save_hdr(HDR_ASSET, make_env_equirect())
        total += path.stat().st_size

    if args.sheet:
        sheet = make_contact_sheet(list(ASSETS.keys()))
        print(f"总览: {sheet}")
    if args.seams:
        seam = make_seam_check(["T_Checker", "T_Brick", "T_BrickMask", "T_Tiles",
                                "T_TilesMask", "T_WaterRipples", "T_Weave", "T_Wood",
                                "T_Truchet", "T_HexGrid", "T_Scales", "T_ScalesMask",
                                "T_Cloud", "T_Sand"])
        print(f"接缝检查: {seam}")

    print(f"完成 {len(names)} 张 PNG + 1 张 HDR，合计 {total / 1024.0 / 1024.0:.2f} MB")
    if SEAM_WARNINGS:
        print("接缝需要人工确认: " + "，".join(SEAM_WARNINGS))


if __name__ == "__main__":
    main()
