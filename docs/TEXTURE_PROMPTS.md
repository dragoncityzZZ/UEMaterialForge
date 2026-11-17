# 默认纹理：AI 提示词 + 文件名

配合 [`DEFAULT_TEXTURES.md`](DEFAULT_TEXTURES.md) 使用。所有文件落在 `<内容根>/assets/textures/`。

> **当前状态：51 张已全部由 [`tools/generate_default_textures.py`](../tools/generate_default_textures.py) 程序化生成**（11.2 MB，可平铺已验证）。
> 本文的提示词现在只用于**换风格**：对木纹、大理石、砖、瓷砖、锈、云、烟、火、沙、鳞片、焦散这 11 张，AI 出的版本通常比噪声程序化更"像实拍"，可以拿来替换同名文件，替换后记得重新 3×3 验平铺并核对色彩空间。

**分两条产线，别混：**

- **A 组（11 张，可选）→ AI 换风格**：有机材质类，提示词在下面，名字即文件名。
- **B 组（40 张）→ 只用程序化**：法线、渐变、棋盘、噪声、遮罩、几何图案。这些是**精确数值数据**，AI 生成的像素不满足数学关系，出了也用不了。

---

## 1. 通用尾缀与负面词

**通用尾缀**（拼在每个正向提示词后面，或设成全局 preset）：

```
seamless tileable texture, perfectly repeating, 1:1 square, orthographic top-down view,
flat uniform lighting, no shadows, no perspective, no vignette, no border, high detail, game-ready
```

**负面词**（SD / Flux 系；MJ 用 `--no`）：

```
text, watermark, logo, signature, border, frame, drop shadow, perspective, 3d scene,
depth of field, vignette, blur, jpeg artifacts, noise, stitched, cut-off edge
```

**参数**：1024×1024、CFG 4–7、steps 28–35；MJ 加 `--tile --ar 1:1 --style raw`。

**遮罩类**（Cloud/Smoke/Fire/Foam/Rust/Dirt/Scratch/Caustics）额外加：`grayscale mask on pure black background, high contrast`。

---

## 2. A 组：AI 换风格提示词（18 条）

其中 **11 条**（Wood / Marble / Brick / Tiles / Scales / Sand / WaterCaustics / Cloud / Smoke / Fire / Rust）对应 `texture_prompts.csv` 里 `mode=generated+ai-alt` 的资产，是当前的换风格候选；另外 7 条（Weave / WaterRipples / WaterFoam / Dirt / NoiseScratch / Energy / StarField）是备选，需要时再用。

### `T_Wood.png` — 1024² · sRGB · 可平铺
```
seamless tileable texture of quarter-sawn oak plank face, straight vertical grain, warm amber
earlywood and dark brown latewood lines, fine fiber detail, matte unfinished surface, photorealistic
```
> 别要"同心年轮"——环状纹理拼不出无缝。要**直纹板面**，纹理方向统一竖直。

### `T_Marble.png` — 1024² · sRGB · 可平铺
```
seamless tileable polished white marble texture, ivory base, fine sharp charcoal and grey veining
with soft translucent halos around each vein, sparse veins, photorealistic stone surface
```

### `T_Brick.png` — 1024² · RGBA · 可平铺
```
seamless tileable red clay brick wall texture, running bond pattern with offset rows, light grey
mortar joints, weathered rough surface with subtle chips and color variation per brick, straight-on view
```

### `T_Tiles.png` — 1024² · RGBA · 可平铺
```
seamless tileable glazed ceramic tile grid, 4x4 white square tiles, thin light grey grout lines,
slightly beveled rounded edges, faint glossy sheen, flat top-down
```

### `T_Scales.png` — 1024² · sRGB · 可平铺
```
seamless tileable reptile scale pattern, overlapping rounded scales in offset rows, deep green to
charcoal gradient, sharp scale edges with thin highlight rims, macro top-down
```

### `T_Sand.png` — 1024² · sRGB · 可平铺
```
seamless tileable fine beach sand texture, top-down macro, warm pale beige grains, subtle wind
ripple bands, extremely fine grain detail, dry loose surface
```

### `T_Weave.png` — 1024² · sRGB · 可平铺
```
seamless tileable fabric macro, 2x2 twill cotton weave, dark navy threads, clearly visible warp and
weft over-under structure, slight thread irregularity, flat top-down
```

### `T_WaterRipples.png` — 1024² · linear · 可平铺
```
seamless tileable water surface seen from directly above, concentric interfering ripple rings,
soft specular highlights on grey-green water, no horizon, no reflections of objects, flat top-down
```
> 只要**俯视水面本身**，不要岸边/天空/倒影物体，否则没法当高度图用。

### `T_WaterCaustics.png` — 1024² · linear · 可平铺
```
water caustics light pattern, bright thin web-like caustic lines on pure black background, grayscale,
high contrast, dense at center fading outward, top-down, no objects
```

### `T_WaterFoam.png` — 1024² · linear · 可平铺
```
white sea foam texture on pure black background, lace-like bubbly foam clusters with feathered edges,
grayscale mask, high contrast, top-down
```

### `T_Cloud.png` — 1024² · sRGB · 可平铺
```
cumulus cloud alpha mask on pure black background, soft wispy billowing edges, grayscale, high
contrast white clouds breaking up at the edges, no sky gradient, flat
```

### `T_Smoke.png` — 512² · sRGB · 不平铺
```
thin vertical smoke strands rising on pure black background, soft feathered translucent wisps,
grayscale, high contrast, flat sheet, no source object
```

### `T_Fire.png` — 512² · sRGB · 不平铺
```
vertical flame tongues on pure black background, orange-yellow fire with bright white hot cores,
soft feathered edges, no logs no embers, flat sheet
```

### `T_Rust.png` — 1024² · sRGB · 可平铺
```
seamless tileable rust corrosion mask, flaking orange-brown patches with pitted corroded edges,
some grey bare metal showing through, macro top-down, high contrast
```

### `T_Dirt.png` — 1024² · linear · 可平铺
```
seamless tileable grime mask, soft grey blotches and speckles on black, low contrast, cloud-like
smudges with no hard edges, flat uniform, grayscale
```

### `T_NoiseScratch.png` — 1024² · linear · 边缘连续
```
fine hairline scratch marks on black, thin light grey scratches in random overlapping directions,
sparse, worn surface macro, high contrast, seamless
```

### `T_Energy.png` — 512² · linear · 不平铺
```
electric lightning arcs, thin white-blue branching bolts on pure black background, glowing bright
cores, branching forks, high contrast, flat sheet
```

### `T_StarField.png` — 1024² · linear · 可平铺
```
star field, small white points of varying brightness on pure black, sparse distribution, a few
larger brighter stars, no nebula, no galaxy glow, no Milky Way band, flat
```

---

## 3. 出图后的四步（不做这步 80% 会用坏）

1. **验平铺**：复制成 3×3 拼一起看。AI 的 `seamless` 大概 50% 不成立。
2. **修缝**（如果拼不上）：图像偏移半幅（Photoshop 滤镜 → 其它 → 位移，水平/垂直各 50%，勾"循环"）→ 接缝跑到画面中间 → 仿制/修补 → 再偏移回来。别用镜像拼接，会毁掉方向性（木纹、水流、条纹全废）。
3. **降采样**：噪声/遮罩类 → 512² 或 256²（1024 的像素级噪声到 512 会变成糊状，反而更可用）；图案/材质 → 保持 1024²。
4. **定色彩空间**：颜色类（Wood/Marble/Brick/Tiles/Scales/Sand/Weave/Cloud/Smoke/Fire/Rust）标 `srgb=true`；遮罩类（Caustics/Foam/Dirt/Scratch/Ripples/Energy/StarField）标 `srgb=false`，并在生成后把灰度拉满对比（levels 拉黑点白点），否则当遮罩用时灰蒙蒙没力度。

**法线仍然全部程序化派生**，包括 `T_Scales`、`T_Brick`、`T_Tiles` 的配套法线——用高度通道求导，别让 AI 画法线。

---

## 4. B 组：只用程序化（40 张，不要用 AI）

| 资产 | 为什么 AI 不行 | 生成方式 |
|---|---|---|
| `T_White` `T_Black` `T_MidGray` `T_FlatNormal` | 必须是精确常量值与 (128,128,255) | 单行代码 |
| `T_Checker` `T_CheckerSmall` `T_UVChecker` `T_Grid` | 必须像素级对齐、格数精确、可数 | 公式 |
| `T_GradientV` `T_GradientRadial` | AI 出渐变必然带状/不均匀，且要 sRGB 编码 | 公式 |
| `T_NoiseWhite` `T_NoisePerlin` `T_NoiseValue` `T_NoiseFBM` `T_NoiseTurbulence` `T_NoiseRidged` `T_NoiseCellular` `T_NoiseWorley` `T_NoiseGrain` `T_NoiseBlue` | 需要可复现的确定性分布，AI 只能给"像噪声的图"，无法参数化、无法保证平铺 | 公式（固定 seed） |
| `T_NoiseCurl` `T_WaterFlow` | 必须是数学上的向量场（无散度） | 由噪声求导 |
| `T_MaskCircle` `T_WaterNormal` `T_WaterWaves` `T_WaterRipples` 的高度通道 | 需精确 SDF / 精确梯度关系 | 公式 |
| `T_StripesV` `T_StripesH` `T_StripesD45` `T_Dots` `T_HexGrid` `T_Truchet` `T_Scanlines` | 几何图案，AI 数不对格数、线宽会抖 | 公式 |
| `T_DefaultEquirect.hdr` | 需要 Radiance HDR + 真实亮度峰值 (>1.0)，AI 只出 LDR 图 | 现成 HDRI 或手搭三点光 |

批量清单（含中文提示词、负面词、目标尺寸）见 [`texture_prompts.csv`](../assets/textures/texture_prompts.csv)。
