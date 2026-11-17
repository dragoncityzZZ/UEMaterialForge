# 默认纹理资产制作清单

MaterialForge 随包的默认贴图库。目标：**内容浏览器一打开就有一套能立刻拿来做材质的东西**，不用先找贴图、不用外链素材。

> **状态：56 张已全部生成完毕**（`assets/textures/`，共 12.5 MB，`default_texture_manifest.csv` 的 `status` 全为 `done`）。
> 由 [`tools/generate_default_textures.py`](../tools/generate_default_textures.py) 确定性生成，固定 seed，重跑不会产生无意义的 diff：
>
> ```
> python tools/generate_default_textures.py            # 全部重新生成
> python tools/generate_default_textures.py --only T_Wood T_NoiseFBM
> python tools/generate_default_textures.py --sheet --seams   # 缩略图总览 + 3x3 接缝检查图
> ```

- 存放位置：`<内容根>/assets/textures/`（安装目录）；工程内 `assets/textures/` 同名文件优先覆盖。
- 现有范例：`T_HoloGrid.png`（1024²，早期由 `tools/generate_textures.py` 生成的电路全息图，保留作演示资产）。
- 跟踪表：[`assets/textures/default_texture_manifest.csv`](../assets/textures/default_texture_manifest.csv)。
- 三批：**P0 骨架 14 张 → P1 噪声/图案 18 张 → P2 水与自然 19 张**，另有 **5 张后补**（4 张通道包 + 1 张彩色编号棋盘）。
- 预览：[总览图](images/_contact_sheet.png) · [3×3 接缝检查](images/_seam_check.png)（`--sheet --seams` 生成，不入 `assets/`，避免污染内容浏览器）。

---

## 1. 规格约定（先定死，否则后面返工）

| 项目 | 约定 | 原因 |
|---|---|---|
| 命名 | `T_` + 驼峰英文，无空格无中文，如 `T_NoisePerlin.png` | 和 `T_HoloGrid` 一致；JSON 里是字面路径，改名会断引用 |
| 尺寸 | 噪声/遮罩 512²，图案/检查器 1024²，渐变 1024×16，常量 8×8 | 全部 2 的幂；1024 以上没必要，预览是球体/平面 |
| 格式 | 8bit PNG（RGBA 或 RGB），`optimize=True` | 加载器走 stb_image，强制 4 通道读取 |
| HDR | 只支持 Radiance `.hdr`（`stbi_loadf`），**不要用 EXR** | `Texture2D::FromHdr` 只认 stb 能解的形式 |
| 色彩 | 颜色类 `srgb=true`，数据类（法线/遮罩/噪声/高度/粗糙度）`srgb=false` | 采样设置是逐贴图的，错了整条链路都偏 |
| 渐变编码 | 要得到线性 0→1 的渐变，存 `round(255*srgb_encode(t))` 并标 `srgb=true`；不要直接存 0..255 拉伸 | 否则渐变在预览里中间调压暗、出现断层 |
| UV 朝向 | 加载时 `stbi_set_flip_vertically_on_load(1)` → **图片底行 = v=0，顶行 = v=1** | 带方向的图（渐变、UV 检查器、水流、箭头）全按这个摆 |
| 平铺 | 所有图案/噪声必须 3×3 拼接无缝 | 频率只能用整数周期，越界索引要 `mod N` |
| 体积 | 单张 ≤ 1.5 MB，全库 ≤ 25 MB | 随包分发 |
| 法线 | OpenGL/UE 约定：**绿通道朝上 (+Y)**，切线空间，`(n*0.5+0.5)` | 与 UE5 Push 的目标管线一致 |

---

## 2. 总表

### P0 — 骨架（14 张，先做这批）

| # | 资产 | 分类 | 主要用途 | 尺寸 | 通道 | 色彩 | 平铺 |
|---|---|---|---|---|---|---|---|
| 1 | `T_White` | 常量 | BaseColor/Emissive 占位、纯色测试 | 8² | RGB | linear | 是 |
| 2 | `T_Black` | 常量 | 遮罩全关、自发光零值 | 8² | RGB | linear | 是 |
| 3 | `T_MidGray` | 常量 | 粗糙度/金属度 0.5 默认 | 8² | RGB | linear | 是 |
| 4 | `T_FlatNormal` | 常量 | Normal 输入默认值 (128,128,255) | 8² | RGB | linear | 是 |
| 5 | `T_Checker` | 检查器 | UV 拉伸/平铺检查、TextureSample 回退 | 1024² | RGB | sRGB | 是 |
| 6 | `T_CheckerSmall` | 检查器 | 细密棋盘，看 mip 与摩尔纹 | 1024² | RGB | sRGB | 是 |
| 7 | `T_UVChecker` | 检查器 | 方向/翻转/坐标系判断（带方向色+角标） | 1024² | RGB | sRGB | 是 |
| 8 | `T_Grid` | 检查器 | 对齐参考、网格类材质底图 | 1024² | RGB | sRGB | 是 |
| 9 | `T_GradientV` | 渐变 | 竖直 alpha/遮罩渐变 | 1024×16 | RGB | sRGB | 是 |
| 10 | `T_GradientRadial` | 渐变 | 径向衰减、圆形淡出 | 1024² | RGB | sRGB | 否 |
| 11 | `T_NoiseWhite` | 噪声 | 白噪声参考、抖动母版 | 512² | RGB | linear | 是 |
| 12 | `T_NoisePerlin` | 噪声 | 单层柏林，最常用的扭曲/遮罩源 | 512² | RGB | linear | 是 |
| 13 | `T_NoiseFBM` | 噪声 | 5 octave fBm，脏污/云母版 | 512² | RGB | linear | 是 |
| 14 | `T_MaskCircle` | 遮罩 | 圆形软边遮罩 | 512² | RGB | linear | 否 |

### P1 — 噪声与图案主体（18 张）

| # | 资产 | 分类 | 主要用途 | 尺寸 | 通道 | 色彩 | 平铺 |
|---|---|---|---|---|---|---|---|
| 15 | `T_NoiseValue` | 噪声 | value noise，比 perlin 更"块" | 512² | RGB | linear | 是 |
| 16 | `T_NoiseWorley` | 噪声 | Worley F1，细胞/裂纹底 | 512² | RGB | linear | 是 |
| 17 | `T_NoiseCellular` | 噪声 | 打包：R=F1 G=细胞随机 B=边缘 A=细胞 ID | 512² | RGBA | linear | 是 |
| 18 | `T_NoiseTurbulence` | 噪声 | abs(fBm)，火焰/云/大理石扭曲 | 512² | RGB | linear | 是 |
| 19 | `T_NoiseRidged` | 噪声 | ridged multifractal，山脊/闪电 | 512² | RGB | linear | 是 |
| 20 | `T_NoiseCurl` | 场 | RG=curl 方向场，B=强度（无散度流动） | 512² | RGBA | linear | 是 |
| 21 | `T_NoiseBlue` | 噪声 | 蓝噪声，抖动/TPDF/抖色 | 256² | RGB | linear | 否 |
| 22 | `T_NoiseGrain` | 噪声 | 胶片颗粒，粗糙度微扰 | 256² | RGB | linear | 是 |
| 23 | `T_NoiseScratch` | 噪声 | 细划痕线，磨损遮罩 | 1024² | RGB | linear | 边缘连续 |
| 24 | `T_StripesV` | 图案 | 竖条纹（软边） | 1024² | RGB | linear | 是 |
| 25 | `T_StripesH` | 图案 | 横条纹 | 1024² | RGB | linear | 是 |
| 26 | `T_StripesD45` | 图案 | 45° 斜条纹 | 1024² | RGB | linear | 是 |
| 27 | `T_Dots` | 图案 | 圆点阵列 | 1024² | RGB | linear | 是 |
| 28 | `T_HexGrid` | 图案 | 六边形蜂窝 | 1024² | RGB | linear | 是 |
| 29 | `T_Brick` | 图案 | 红砖墙：错缝 + 灰缝 + 每砖色差 ±20% | 1024² | RGB | sRGB | 是 |
| 30 | `T_Tiles` | 图案 | 白瓷砖 + 圆角倒角灰缝 | 1024² | RGB | sRGB | 是 |
| 31 | `T_Truchet` | 图案 | Truchet 弧线拼接，图案母版 | 1024² | RGB | linear | 是 |
| 32 | `T_Weave` | 图案 | 2/2 斜纹织物 | 1024² | RGB | linear | 是 |

### P2 — 水 / 自然 / 特效（19 张）

| # | 资产 | 分类 | 主要用途 | 尺寸 | 通道 | 色彩 | 平铺 |
|---|---|---|---|---|---|---|---|
| 33 | `T_WaterRipples` | 水 | 水波高度（灰度，中频段） | 1024² | RGB | linear | 是 |
| 34 | `T_WaterNormal` | 水 | 由 #33 求导的法线（绿朝上） | 1024² | RGB | linear | 是 |
| 35 | `T_WaterCaustics` | 水 | 焦散网纹 | 1024² | RGB | linear | 是 |
| 36 | `T_WaterFlow` | 水 | RG=流向场，B=速度 | 512² | RGBA | linear | 是 |
| 37 | `T_WaterFoam` | 水 | 泡沫/浪花遮罩 | 1024² | RGB | linear | 是 |
| 38 | `T_WaterWaves` | 水 | 大尺度正弦波叠加（整数频率，可平铺） | 1024² | RGB | linear | 是 |
| 39 | `T_Wood` | 自然 | 年轮木纹 | 1024² | RGB | sRGB | 是 |
| 40 | `T_Marble` | 自然 | 大理石细脉 | 1024² | RGB | sRGB | 是 |
| 41 | `T_Rust` | 自然 | 锈蚀：灰金属底 + 橙褐锈斑 | 1024² | RGB | sRGB | 是 |
| 42 | `T_Dirt` | 自然 | 污渍/灰尘（低对比） | 1024² | RGB | linear | 是 |
| 43 | `T_Cloud` | 自然 | 云 alpha | 1024² | RGBA | sRGB | 是 |
| 44 | `T_Smoke` | 自然 | 烟 alpha（各向异性） | 512² | RGBA | sRGB | 否 |
| 45 | `T_Fire` | 自然 | 火焰温度/alpha | 512² | RGBA | sRGB | 否 |
| 46 | `T_Sand` | 自然 | 沙粒 + 沙丘条纹 | 1024² | RGB | sRGB | 是 |
| 47 | `T_Scales` | 自然 | 鳞片（含配套法线） | 1024² | RGB | linear | 是 |
| 48 | `T_Energy` | 特效 | 分支闪电/能量线 | 512² | RGB | linear | 否 |
| 49 | `T_StarField` | 特效 | 星空点阵 | 1024² | RGB | linear | 是 |
| 50 | `T_Scanlines` | 特效 | 扫描线（全息系列配套） | 512² | RGB | linear | 是 |
| 51 | `T_DefaultEquirect.hdr` | 环境 | 预览球默认 HDRI（3 点光 + 灰底） | 1024×512 | RGB float | linear | 否 |

> `T_HoloGrid` 保留为演示资产，不参与"默认值"用途。

### 后补（5 张）

| # | 资产 | 分类 | 主要用途 | 尺寸 | 通道 | 色彩 | 平铺 |
|---|---|---|---|---|---|---|---|
| 52 | `T_BrickMask` | 通道包 | R=砖面倒角高度 G=每砖 ID B=遮盖 A=灰缝 | 1024² | RGBA | linear | 是 |
| 53 | `T_TilesMask` | 通道包 | R=砖面 G=每瓦 ID B=倒角 A=灰缝 | 1024² | RGBA | linear | 是 |
| 54 | `T_RustMask` | 通道包 | R=锈量 G=裸露金属 B=坑洞 A=1 | 1024² | RGBA | linear | 是 |
| 55 | `T_ScalesMask` | 通道包 | R=鳞壳 G=每片 ID B=边缘高光 A=1 | 1024² | RGBA | linear | 是 |
| 56 | `T_ColorChecker10x10` | 检查器 | 彩色编号棋盘 A1–J10，1:1 参考图 | 2000² | RGB | sRGB | 否 |

通道包的用法：`ComponentMask`（R/G/B/A 各取一路）分别驱动"每块砖随机偏移 / 遮盖 / 灰缝高光"，一张图解决随机化，不用再画第二张。数据类一律 `srgb=false`，取出来是原始 0–1。

---

## 3. 逐条制作要点

### 3.1 常量（1–4）

- 8×8 而不是 1×1：避免个别后端 1px 纹理的采样/上传路径走到奇怪分支，也让 mip 链正常。
- `T_MidGray` 存 128、`srgb=false` → 采样得 0.502 线性，正是"中灰"。
- `T_FlatNormal` 存 (128,128,255)、`srgb=false`。

### 3.2 检查器（5–8）

- `T_Checker`：8×8 格；深 `#3A3A3A`、浅 `#C8C8C8`（中性灰，不用红蓝——红蓝会盖掉材质本身的颜色判断）。
- `T_CheckerSmall`：16×16 格，用途是看 mip 过滤和摩尔纹。
- `T_UVChecker`：8×8 格。每格底色按格索引给方向色（格列→红递增、格行→绿递增），格内左下角画小三角指向 `+U`（红）/`+V`（绿），外框 2px 深灰、**左上角单独一格挖空**用于判断翻转。
- `T_Grid`：1/16 间距的 1px 细线 + 1/4 间距的 2px 主线，底色 `#101010`，线 `#E0E0E0`。

### 3.3 渐变（9–10）

`t` 定义为线性目标值，写入时按 `round(255*srgb_encode(t))`，`srgb=true`：

- `T_GradientV`：`t = v`（v=0 在图片底部，所以底黑顶白）。
- `T_GradientRadial`：`t = 1 - clamp(length(uv*2-1), 0, 1)`；wrap 设 clamp，不要拼贴。

### 3.4 噪声（11–23）

统一：单通道算完后复制到 RGB，A=255（除打包类）；输出归一化到 `[0,1]` 且做一次 0.5% 百分位拉伸，避免整体发灰。

可平铺的通用做法——格点索引 `mod N`，频率只取整数：

```
hash(x, y) = fract(sin(x*127.1 + y*311.7) * 43758.5453)
```

- `T_NoiseWhite`：逐像素均匀随机 `rng.random()`，seed 固定。
- `T_NoisePerlin`：周期网格 `N=8`，quintic 插值 `6t⁵-15t⁴+10t³`。
- `T_NoiseFBM`：起始频率 4、5 层、lacunarity 2、gain 0.5，权重归一到 1；**每层频率都是整数**才能平铺。
- `T_NoiseValue`：同上但格点取随机标量 + 双线性/quintic 插值。
- `T_NoiseWorley`：8×8 抖动格，`jitter=1.0`，跨边界查 9 邻格并 `mod 8`；输出 F1 距离。
- `T_NoiseCellular`：R=F1，G=最近细胞 hash，B=`clamp(F2-F1)`，A=细胞 ID/255。
- `T_NoiseTurbulence`：`Σ gain^i * |perlin(f_i)|`，5 层后归一化。
- `T_NoiseRidged`：`Σ gain^i * (1-|perlin(f_i)|)²`，归一化。
- `T_NoiseCurl`：对周期高度场 `h` 做中心差分，`c = (∂h/∂v, -∂h/∂u)`，存 `0.5+0.5*normalize(c)`，B 存 `length(c)` 归一化。
- `T_NoiseBlue`：256²，void-and-cluster / 能量最小化，环形距离（不要求拼贴，但用环形距离收敛更快）。
- `T_NoiseGrain`：高斯 `σ≈0.06` 叠在 0.5 上，再按 `y` 加 ±2% 的轻微行相关。
- `T_NoiseScratch`：随机线段（长 8–64px，宽 1–3px，方向集中在两个主方向），**画到画布时按 wrap 复制到对边**，保证左右上下接缝不出现突兀断裂。

### 3.5 图案（24–32）

- `T_StripesV/H`：8 条，`smoothstep(0.45,0.55,frac(k*8))` 两侧对称，得到软边条纹。
- `T_StripesD45`：用 `frac((u+v)*8)`——`u+v` 周期为 1，所以平铺天然无缝。
- `T_Dots`：8×8 圆点，半径 0.35 格，边缘 1.5px 抗锯齿。
- `T_HexGrid`：8 列 × 7 行平顶六边形。用六边形 SDF（apothem = 行距的一半）取等值线，奇偶列垂直错开半行；**列数必须取偶数**，否则奇偶偏移在 x 方向接缝处对不上。
- `T_Brick`：8 行 × 4 列（1024 下每砖 256×128），奇数行横移 0.5 格错缝，灰缝 6px、边缘按倒角压暗；每砖用格 hash 做 ±20% 明度/色相抖动，再叠一层 fBm 颗粒。
- `T_Tiles`：4×4 方砖，圆角 12px 倒角，砖面受光提亮、灰缝压暗，抗锯齿用 SDF 软边。
- `T_Truchet`：8×8 格，每格 hash 二选一（两条半圆弧 / 两条直角弧），线宽 8px，格索引整数所以边界连续。
- `T_Weave`：`(i+j) mod 4 < 2` 决定经线在上，R=高度（上方的线更亮，下方压暗 25%）。

### 3.6 水（33–38）

这是这套库的重点，注意**高度与法线必须同源**，不要分开手绘：

- `T_WaterRipples`：在 torus 上取 6 个随机波源 `c_i`，三组频率分别求和

  ```
  h_k(p) = Σ_i sin(2π*(f_ik*|p ⊖ c_i| - φ_ik))
  ```

  `|·|` 用环形最短距离（`min(|d|, 1-|d|)`），所以拼贴无缝。三档频率 `3 / 7 / 15` 均归一化到 `0.5±0.35`，**最终只把中频段写进 RGB 三个通道**——灰度更像水，法线也从同一段派生，三档数据留给以后做变体。
- `T_WaterNormal`：取 #33 的中频段，环形差分求梯度，`n = normalize(vec3(-∂h/∂u*k, -∂h/∂v*k, 1))`，`k≈1.6`，存 `n*0.5+0.5`，**绿朝上**。k 别开大，否则整张图法线全歪、看着像彩色噪声。
- `T_WaterCaustics`：`pow(1-clamp((F2-F1)/w), 3)`（w≈0.16）再乘一层大尺度径向衰减，得到网状亮线。
- `T_WaterFlow`：取 curl 场做基底，混入 15% 向心分量，RG 存 `0.5+0.5*normalize(v)`，B 存速度 0.35–0.8。
- `T_WaterFoam`：fBm 团簇（`smoothstep(0.48,0.70)`）叠 worley 气泡边缘，沿 v 方向 3 倍拉伸——**拉伸倍数必须是整数**，否则垂直接缝会断。
- `T_WaterWaves`：4 个方向的正弦叠加，方向取 `(cos θ_k, sin θ_k)` 且 `θ_k` 为 `0/45/90/135`，频率整数 → 平铺正确；输出 `0.5+0.5*h`。

### 3.7 自然（39–47）

- `T_Wood`：`t = frac(r*f + 0.35*fbm)`，`r` 为环形距离（从圆心向外），`f≈9`；早材暖色 `#8A6337`、晚材深色 `#4A3520`，年轮边界 1px 深线。
- `T_Marble`：`t = sin(2π*(u*3 + 2.4*turbulence))`，`1-smoothstep(0, 0.22, |t|)` 做细脉，基色 `#D8D4CC`、脉色近黑。
- `T_Rust`：fBm `smoothstep(0.38,0.58)` 定锈斑范围，叠竖向 streak 与细坑洞；灰金属底 `#484A4E` 与锈色 `#4C2110`→`#A35120` 之间插值。
- `T_Dirt`：低对比 fBm（0.30–0.64）+ 低频斑块 + 细颗粒，**别拉高对比**，它是要当遮罩用的。
- `T_Cloud`：6 层 fBm，`smoothstep(0.44,0.74)` 软过渡，RGB 白、A=覆盖率；不要加垂直窗口，否则上下边缘会出现一条暗带。
- `T_Smoke`：域扭曲（warp fBm 偏移采样坐标）+ 垂直衰减 + 水平收边；不平铺，用 clamp。
- `T_Fire`：噪声沿 `v` 拉伸 + 底部强顶部弱，热核提亮成白黄，A 存 alpha。
- `T_Sand`：细颗粒（freq 96 的 fBm）+ 低频沙丘条纹（`sin(2π(2v + 0.3*fbm))`）；波纹权重压到 0.07，否则会变成人字纹。
- `T_Scales`：14 行 × 10 列，隔行横移半格，圆弧壳 + 边缘高光 rim；法线由壳高度派生（可选）。

### 3.8 特效与环境（48–51）

- `T_Energy`：随机游走画主干（步长 1.6px、带角度抖动）→ 每 90 步分叉一次 → 两级 box blur 叠辉光 → `pow(0.65)` 提亮。
- `T_StarField`：3600 个点，亮度按幂律（`r^2.4`），少数大星补 5 点十字，最后叠一层 halo；跨边界用 `mod` 复制。
- `T_Scanlines`：`0.5+0.5*sin(v*2π*N)`（N=64，整数 → 平铺）+ 每 8 行加粗 + 1.2% 行噪声。
- `T_DefaultEquirect.hdr`：1024×512 Radiance RGBE，三分渐变天空（上冷蓝、地平线暖灰、下中性灰）+ 3 个高斯亮斑当前景灯（主灯峰值约 12），用于高光与 PBR 测试。手写 RGBE 头，读的时候走 `stbi_loadf`。

### 3.9 通道包与编号棋盘（52–56）

**通道包**：和对应的材质图**用同一个 `*_fields()` 函数算中间量**，只是输出通道不同——改了砖的形状，两张图一起变，不会对不上。

| 资产 | R | G | B | A |
|---|---|---|---|---|
| `T_BrickMask` | 砖面倒角高度（灰缝=0） | 每砖 ID（0–1，逐砖随机） | 砖面遮盖 | 灰缝遮盖 |
| `T_TilesMask` | 砖面遮盖 | 每瓦 ID | 倒角高度 | 灰缝遮盖 |
| `T_RustMask` | 锈量 | 裸露金属 | 坑洞/粗糙度扰动 | 1 |
| `T_ScalesMask` | 鳞壳遮盖 | 每片 ID | 边缘高光（rim） | 1 |

**`T_ColorChecker10x10`**：2000×2000，10×10 格，每格 200px，1:1。

- 配色：色相沿列 `+36°`、沿行 `+14°` 推进，奇偶格在「浓彩 S=0.78」与「淡彩 S=0.16」之间切换——所以既是棋盘（相邻反差明显），又是一张彩色渐变图。
- 编号：**列字母 A–J 从左到右，行号 1–10 从上到下，A1 在左上角**。空格中心用粗体居中，字色按格子亮度自动切换黑白（阈值 0.55）。字体优先 `arialbd.ttf`，找不到退 `seguisb/arial/segoeui`，再退位图字体。
- 2px 深色分格线，线条压在格线上不占格。
- **不平铺**（`tile=False`）：它是参考图，不是贴图。要当 UV 测试图用的话，10 格偶数、奇偶与色相都以 10 为周期，几何上其实可以拼，只是接缝处会出现 4px 双线；需要无缝版本告诉我，把分格线改成只画内部线即可。

---

## 4. 派生与配对关系（别各画各的）

| 源 | 派生 | 规则 |
|---|---|---|
| 任一高度图 `H` | 法线 | `n = normalize(vec3(-∂H/∂u*k, -∂H/∂v*k, 1))`，绿朝上，`k` 记在文件名旁的注释里（建议 1.5–3.0） |
| `T_WaterRipples`（中频段） | `T_WaterNormal` | 同尺寸、同 seed、同频率 |
| `T_Brick`/`T_Tiles`/`T_Scales` 的倒角/壳高度 | 对应法线 | 只在需要时出，避免库里法线爆炸 |
| `T_NoiseCurl` | `T_WaterFlow` | 流向场必须无散度，直接从 curl 来 |

一句话：**高度先行，法线派生；同一份数据能复用的就别再画一张。**

---

## 5. 接入清单（贴图已就位，剩接线）

1. **命名与路径**：全部平铺放在 `assets/textures/`，文件名即资产名，JSON 里写字面相对路径 `assets/textures/T_NoiseFBM.png`。工程内同名文件优先，其次是内容根——所以默认贴图放内容根即可被所有工程解析到。（已完成）
2. **节点默认值（已改）**：`src/core/NodeRegistry.cpp` 的 `TextureObject.texture`（953 行）、`TextureObjectParameter.texture`（969 行）、`TextureSampleParameter2D.texture`（1020 行）已从 `T_HoloGrid.png` 改为 `assets/textures/T_Checker.png`——回退语义就是棋盘，默认值现在和语义一致；`T_HoloGrid` 退回演示用途（示例图与示例工程仍引用它）。
3. **区分两份棋盘**：代码内的回退棋盘是 `Texture2D::FromCheckerboard`（`src/io/TextureLoader.cpp:144`，64px、8×8、红蓝配色），只用于加载失败兜底；资产版 `T_Checker` 是中性灰、给用户用的。两者不要混。

### 生成脚本

实现见 [`tools/generate_default_textures.py`](../tools/generate_default_textures.py)：一个资产一个 `make_*` 函数，末尾一张 `ASSETS` 注册表描述每个资产的 `srgb / display / tileable`，`save()` 负责编码、8bit 量化、写 PNG，并对可平铺资产做接缝比检查。

```python
ASSETS = {
    "T_Checker":      dict(build=make_checker, kwargs=dict(cells=8), srgb=True,  display=True, tile=True),
    "T_NoisePerlin":  dict(build=make_noise_perlin,                  srgb=False, display=True, tile=True),
    "T_WaterRipples": dict(build=make_water_ripples,                srgb=False, display=True, tile=True),
}
```

三个开关的含义，写新资产时照着填：

| 开关 | 含义 |
|---|---|
| `srgb=True` + `display=False` | 在**线性空间**算完再编码成 sRGB 存储（渐变、需要正确加色混合的图） |
| `srgb=True` + `display=True` | 数值本来就在显示空间，直接写（材质色、检查器） |
| `srgb=False` | 数据图（法线/遮罩/噪声/高度），原样写 |

接缝检查用「接缝跳变 ÷ 内部相邻像素跳变」，比值 ≲ 1 视为无缝。它只做报警不做拦截：棋盘、条纹这类分区常量图内部跳变本来就可能是 0，而 `T_Weave` 的接缝恰好在经纬线边界上（合法跳变），比值天然偏大。**真正的验收是 `--seams` 出的 3×3 拼贴图。**

---

## 6. 验收标准

- [x] 3×3 拼贴看不到接缝（[接缝检查图](images/_seam_check.png) 已过一遍：棋盘/砖/瓷砖/水波/织物/木纹/Truchet/六边形/鳞片/云/沙/噪声全部连续）
- [x] 尺寸为 2 的幂；噪声类 ≤ 512，检查器/图案 1024
- [x] 颜色类标 sRGB、数据类标 linear，同一张图不会两种用途混用
- [x] 渐变没有暗部压死/断层（sRGB 编码后中间调均匀）
- [x] 法线绿朝上，`T_WaterNormal` 与 `T_WaterRipples` 同源
- [x] 单张 ≤ 2.0 MB（最大 `T_Sand` 995 KB），全库 12.5 MB ≤ 25 MB
- [x] `default_texture_manifest.csv` 的 `status` 全部 `done`（56 行）
- [ ] 内容浏览器里按名称排序后分组清晰（`T_Checker* / T_Gradient* / T_Mask* / T_Noise* / T_Stripes* / T_Water*`）——需要开应用确认

---

## 7. 后续可选

- MatCap 一组（4 张球面光照），用于快速看高光形状。
- 纹理集（Texture Set）命名：`T_Brick_BaseColor / _Normal / _Roughness`，如果需要走 UE 传统路线，再从打包版拆。
- 程序化节点替代：`Noise` 节点已内置确定性 3D value noise，部分噪声贴图可以不出资产，直接在材质里接——**只有需要"贴图形式的噪声"（比如给 UE 采样）时才出图**。
- ~~通道包变体~~ 已完成：`T_BrickMask` / `T_TilesMask` / `T_RustMask` / `T_ScalesMask`。
- 砖/瓷砖/鳞片的配套法线还没出（目前靠倒角高度在材质里算），需要的话在 `*_fields()` 的 `bevel` 上求导即可。
