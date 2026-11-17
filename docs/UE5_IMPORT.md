# UE5 Python 导入 / 推送 / 拉取指南

MaterialForge 导出的 JSON 可以直接在 UE5 编辑器里重建材质。桥接脚本基于
`unreal.MaterialEditingLibrary`，不依赖第三方 UE 插件。

## 0. 从 UE 材质编辑器直接粘贴节点

除 JSON 桥接外，MaterialForge 也能直接读取 UE 材质编辑器的**复制文本**：

1. 在 UE4/UE5 材质编辑器中框选节点，按 `Ctrl+C`；
2. 切回 MaterialForge，鼠标指向画布，按 `Ctrl+V`；
3. 节点、属性、未连接输入的默认值和节点之间的连线会按 UE 布局落在鼠标处。

也可以在画布右键的创建节点弹窗顶部点击 **Paste UE5 Material Nodes**。支持
常量、标量/向量参数、TextureObject/TextureSample、TextureCoordinate、Panner、
Rotator、Fresnel、ComponentMask、SceneTexture、Comment、常见数学节点和
Material Output。当前不支持 MaterialFunctionCall、Custom 等需要外部资源或动态
引脚的表达式；这些节点会被跳过，并在状态栏列出被跳过的 UE 类名。UE 的
TextureSample 直接引用贴图资产时，MaterialForge 会自动补一个 TextureObject
节点；贴图路径使用 `/Game/...` 资产路径，本地预览需要手动改为工程内贴图。
UE 通常在复制节点时不会带上 `MaterialGraphNode_Root`，因此原图中连接到材质根
的连线没有可还原的目标属性；导入后状态栏会提示未恢复的根连线数量，需要把末端
节点手动接到 Base Color、Emissive Color、World Position Offset 等 Material
Output 输入。


## 1. 准备贴图

MaterialForge 不把贴图二进制嵌入 JSON。它只保存 `TextureObject.texture` 的
文件路径，例如：

```json
"assets/textures/T_HoloGrid.png"
```

在 UE5 中：

1. 把图片导入 Content Browser，例如放到 `/Game/MaterialForge/Textures/`；
2. 复制一份贴图映射表（仓库内有示例）：

   [tools/ue5/texture_map.example.json](../tools/ue5/texture_map.example.json)

   ```json
   {
     "assets/textures/T_HoloGrid.png": "/Game/MaterialForge/Textures/T_HoloGrid"
   }
   ```

脚本查贴图的顺序：

1. 贴图映射表中按完整路径、文件名、文件名去扩展名查找；
2. `/Game/MaterialForge/Textures/<stem>`；
3. `/Game/Textures/<stem>`；
4. `/Game/<stem>`。

如果都没找到，会 log warning，并跳过该 TextureSample 的 `texture` 设置；其余
节点仍会导入，只是该贴图需要手动指定。

## 2. 运行导入脚本

在 UE5 编辑器中：

1. 打开 **Window → Output Log**；
2. 将命令类型切换为 **Python**（或使用 `py` 命令）；
3. 执行：

```text
py "C:/dev/noise_shader/tools/ue5/import_material_graph.py" "C:/dev/noise_shader/examples/graphs/hologram_circuit.json" --dest-path /Game/MaterialForge/Materials --texture-map "C:/dev/noise_shader/tools/ue5/texture_map.example.json"
```

可选参数：

| 参数 | 默认 | 说明 |
|---|---|---|
| `--dest-path` | `/Game/MaterialForge/Materials` | 材质所在 Content Browser 路径。脚本会尝试创建目录。 |
| `--material-name` | JSON 中的 `material.name` | 覆盖目标材质资产名。 |
| `--texture-map` | 同目录的 `texture_map.json`（若存在） | 贴图映射 JSON。 |
| `--dry-run` | off | 只校验和打印计划，不修改 UE 资产。 |

### 在 UE 编辑器外做 dry-run

不导入 UE 模块时也可以验证文档与脚本语法：

```powershell
python tools/ue5/import_material_graph.py examples/graphs/hologram_circuit.json --dry-run
```

预期输出类似：

```text
[MaterialForge] Graph: .../hologram_circuit.json
[MaterialForge] Material: M_HoloCircuit
[MaterialForge] Nodes: 14, links: 14, texture samples: 1
[MaterialForge] Dry run complete; no UE assets were modified.
```

## 2.1 Push（MaterialForge → UE）

`push_material.py` 是 `import_material_graph.py` 的对称入口，参数完全一致：

```text
py "C:/path/to/tools/ue5/push_material.py" "C:/path/to/M_Example.json" --dest-path /Game/MaterialForge/Materials --texture-map "C:/path/to/tools/ue5/texture_map.json"
```

也可以从 MaterialForge 的 **工程 → UE5 Push / Pull...** 复制完整命令。

## 2.2 Pull（UE → MaterialForge）

在 UE5 编辑器的 Python 环境中执行：

```text
py "C:/path/to/tools/ue5/pull_material_graph.py" --material /Game/Materials/M_Example --out "C:/path/to/M_Example.pulled.json" --catalog "C:/path/to/tools/ue5/node_catalog.json"
```

可选参数：

| 参数 | 默认 | 说明 |
|---|---|---|
| `--material` | 必填 | UE 材质资产路径。 |
| `--out` | 空 | 输出 MaterialForge JSON 路径；为空时只打印摘要。 |
| `--catalog` | 脚本同目录 `node_catalog.json` | 节点类型/引脚目录。 |
| `--texture-map` | 无 | 复用 Push 的贴图映射表，反向转换为本地文件路径。 |
| `--dry-run` | off | 打印节点/连线计划，不写文件。 |

`node_catalog.json` 由 MaterialForge 生成：

```powershell
MaterialForge.exe --export-node-catalog tools/ue5/node_catalog.json
```

它包含全部 91 个节点类型、`ueClass`、输入/输出引脚和属性默认值，因此
UE class 到 MaterialForge 类型的映射不需要在 Python 里重复维护。

Pull 支持常量、标量/向量/纹理参数、TextureObject/TextureSample、UV、Panner、
Rotator、Fresnel、常见数学节点、Comment 和 Material Output 的 16 个属性。
不支持的 UE 表达式会写入 `source.skippedClasses` 并 log warning，相关连线会被
丢弃而不是猜测。

Pull 的已知限制：

- UE 的 `FExpressionInput` 只暴露输入节点，不暴露完整输出索引；脚本优先选择
  `RGB`，否则选择节点目录中的第一个输出。需要特定通道时请在 MaterialForge 中
  手动改接。
- 没有 `--texture-map` 时，纹理属性会写入 UE 资产路径（`/Game/...`），JSON 可
  原样 Push 回 UE，但 MaterialForge 本地预览无法加载该路径。
- 参数的 Group / Sort Priority / Slider 等 MaterialForge 元数据在 UE 中没有等价
  字段，拉取后使用默认值。
- 当前不支持 FunctionCall、Custom、Landscape 等复杂表达式；它们会被跳过。

## 2.3 批量 Push / Pull

在 MaterialForge 的桥接窗口点击 **Export Project Batch...**，生成：

```json
{
  "schema": "materialforge.ue5.batch",
  "version": 1,
  "destPath": "/Game/MaterialForge/Materials",
  "materials": [
    "C:/.../projects/hologram_demo/materials/M_HoloCircuit.json"
  ]
}
```

在 UE5 Python 环境运行：

```text
py "C:/path/to/tools/ue5/push_batch.py" "C:/path/to/ue_batch.json" --texture-map "C:/path/to/tools/ue5/texture_map.json"
```

批量拉取一个 UE 文件夹：

```text
py "C:/path/to/tools/ue5/pull_batch.py" --folder /Game/Materials --out-dir "C:/path/to/pulled" --catalog "C:/path/to/tools/ue5/node_catalog.json"
```

`pull_batch.py` 会为每个材质生成一个 JSON，并写出
`batch_pull_manifest.json`，记录成功/失败和输出路径。

两个批量脚本都支持 `--dry-run`，批量 Push 在离线环境可校验所有图 JSON。

## 2.4 UE 同步状态

工程目录下的 `.materialforge/ue_sync.json` 记录材质与 UE 资产的对应关系：

```json
{
  "schema": "materialforge.ue_sync",
  "version": 1,
  "materials": {
    "materials/M_HoloCircuit.json": {
      "uePath": "/Game/MaterialForge/Materials/M_HoloCircuit",
      "state": "exported",
      "updatedAt": "2026-10-31T19:56:18"
    }
  }
}
```

- 批量导出清单会把材质标记为 `exported`。
- 桥接窗口的 **Mark Pushed** / **Mark Pulled** 可手动确认同步方向。
- 内容浏览器材质瓦片的 `UE` 徽章表示存在同步记录，悬停显示路径和时间。
- 该文件按工程相对路径保存，不写入 `.mfproject` 主清单，工程移动后仍有效。

## 3. 脚本执行了什么

1. 读取 / 校验 schema 与版本；
2. 解析贴图映射；
3. 若目标材质已存在，加载并调用
   `MaterialEditingLibrary.delete_all_material_expressions`；否则用
   `AssetTools.create_asset(..., unreal.MaterialFactoryNew())` 创建；
4. 设置 `material_domain` / `blend_mode` / `shading_model` / `two_sided` /
   `opacity_mask_clip_value` / `translucency_lighting_mode` 以及 LOD/阴影标志；
5. 按 `ueClass` 创建节点。MaterialOutput 不创建（材质根即输出），
   TextureObject 不创建（贴图被展开到 TextureSample 的资产属性）；
6. 设置节点属性：

   | MaterialForge type | UE 属性 |
   |---|---|
   | Constant | `r` |
   | Constant2Vector | `r`, `g` |
   | Constant3Vector / Constant4Vector | `constant` |
   | ScalarParameter | `parameter_name`, `default_value` |
   | VectorParameter | `parameter_name`, `default_value` |
   | TextureCoordinate | `u_tiling`, `v_tiling` |
   | Panner | `speed_x`, `speed_y` |
   | Rotator | `center_x`, `center_y`, `speed` |
   | Fresnel | `exponent`, `base_reflect_fraction` |
   | TextureSample | `texture`（从 TextureObject 连线解析） |

7. 恢复内部连线：
   `connect_material_expressions(from, output_name, to, input_name)`；
8. 恢复到材质根的连线：
   `connect_material_property(from, output_name, unreal.MaterialProperty.MP_...)`；
9. `recompile_material`，保存资产。

## 4. 输入 / 输出名称映射

| JSON pin name | UE 连接名 |
|---|---|
| `Base Color` | `MaterialProperty.MP_BASE_COLOR` |
| `Metallic` | `MP_METALLIC` |
| `Specular` | `MP_SPECULAR` |
| `Roughness` | `MP_ROUGHNESS` |
| `Emissive Color` | `MP_EMISSIVE_COLOR` |
| `Normal` | `MP_NORMAL` |
| `Ambient Occlusion` | `MP_AMBIENT_OCCLUSION` |
| `Opacity` | `MP_OPACITY` |
| `Opacity Mask` | `MP_OPACITY_MASK` |
| `World Position Offset` | `MP_WORLD_POSITION_OFFSET` |
| `Pixel Depth Offset` | `MP_PIXEL_DEPTH_OFFSET` |
| `Refraction` | `MP_REFRACTION` |
| `Clear Coat` | `MP_CLEAR_COAT` |
| `Clear Coat Roughness` | `MP_CLEAR_COAT_ROUGHNESS` |
| `Subsurface Color` | `MP_SUBSURFACE_COLOR` |
| `Anisotropy` | `MP_ANISOTROPY` |

内部输入名按以下顺序解析：

1. 查询 UE 当前的输入引脚名（`get_material_expression_input_names`）；
2. 精确匹配 JSON 的 `toPinName`；
3. 别名匹配（例如 `UVs` ↔ `Coordinates`、`Texture` ↔ `Tex` /
   `TextureObject`）；
4. 如果只有单个输入，使用该唯一引脚。

若最终仍找不到输入，会输出 warning 并跳过该连线，不会中断整个导入。

## 5. 兼容性检查清单

在目标 UE 项目中第一次运行时，建议检查：

- [ ] 目标 `MaterialExpression` 类在 Python 中可访问
  （脚本用 `getattr(unreal, ueClass)`，缺失时会警告并跳过）；
- [ ] 贴图映射表指向的资产真实存在；
- [ ] 打开材质编辑器确认布线及参数；
- [ ] 检查 TextureSample 的 Sampler Type / 色彩空间；
- [ ] 若用到 Panner Time 引脚，确认目标 UE 版本暴露了该输入；
- [ ] 检查材质 Blend Mode / Shading Model 是否符合预期。

## 6. 已知限制

- 脚本针对 UE5 的 `MaterialEditingLibrary`。它只在 Editor / Editor Scripting
  环境可用，不能在打包后的游戏运行时执行。
- 不同 UE5 版本的 Python 暴露面和属性名可能略有差异；脚本对属性和输入名做了
  defensive 处理，但新增节点仍需在目标版本实测。
- MaterialForge 的预览光照、纹理色彩空间、Fresnel 等是近似；以 UE 内结果为准。
- JSON 中的数字节点 ID 可能在重新保存后变化；桥接不依赖它在多个文件间保持
  恒定。
- 目标材质已存在时会清空其表达式并重建。建议先提交/备份该资产，或在
  `--material-name` 中指定新名字。
- 当前没有自动导入图片文件的步骤；请先在 UE 中导入贴图并维护映射。

## 参考

- [unreal.MaterialEditingLibrary — Unreal Python 5.4](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/MaterialEditingLibrary?application_version=5.4)
- [unreal.MaterialExpressionScalarParameter](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/MaterialExpressionScalarParameter?application_version=5.4)
- [unreal.MaterialExpressionVectorParameter](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/MaterialExpressionVectorParameter?application_version=5.4)
- [unreal.MaterialExpressionTextureSample](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/MaterialExpressionTextureSample?application_version=5.4)
- [unreal.MaterialExpressionPanner](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/MaterialExpressionPanner?application_version=5.4)
- [unreal.MaterialExpressionTextureCoordinate](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/MaterialExpressionTextureCoordinate?application_version=5.4)
- [unreal.MaterialExpressionFresnel](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/MaterialExpressionFresnel?application_version=5.4)
- [unreal.MaterialExpressionRotator](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/MaterialExpressionRotator?application_version=5.4)
