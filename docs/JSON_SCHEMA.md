# JSON Schema v1

MaterialForge 的材质图文件同时是 UE5 导入桥接文档。

```json
{
  "schema": "materialforge.ue5.material",
  "version": 1,
  "generator": "MaterialForge 1.0",
  "engine": "Unreal Engine 5",
  "material": { ... },
  "nodes": [ ... ],
  "links": [ ... ]
}
```

完整示例：[examples/graphs/hologram_circuit.json](../examples/graphs/hologram_circuit.json)。

## 顶层字段

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `schema` | string | 是 | 固定为 `materialforge.ue5.material`。 |
| `version` | integer | 是 | 当前为 `1`。加载器拒绝其他版本。 |
| `generator` | string | 否 | 写入方标识；仅作信息。 |
| `engine` | string | 否 | 固定 `Unreal Engine 5`。 |
| `material` | object | 否（缺省时用默认值） | 材质元数据。 |
| `nodes` | array | 是 | 节点实例，必须包含且仅包含一个 `MaterialOutput`。 |
| `links` | array | 是 | 有向连线：output pin → input pin。 |

## material

```json
"material": {
  "name": "M_HoloCircuit",
  "domain": "Surface",
  "blendMode": "Opaque",
  "shadingModel": "DefaultLit",
  "twoSided": false
}
```

| 字段 | 说明 |
|---|---|
| `name` | 材质资产名。工具栏和保存对话框的默认文件名源自它。 |
| `domain` | UE 材质 domain，目前预览固定为 Surface 语义。 |
| `blendMode` | `Opaque` / `Masked` / `Translucent` / `Additive` / `Modulate` / `AlphaComposite`。UE 导入映射为 `unreal.BlendMode`。 |
| `shadingModel` | `DefaultLit` / `Unlit` / `Subsurface` / ...。UE 导入映射为 `unreal.MaterialShadingModel`。 |
| `twoSided` | 预览是否关闭背面剔除，并导出 UE `two_sided`。 |

## nodes[]

```json
{
  "id": 7,
  "type": "VectorParameter",
  "ueClass": "MaterialExpressionVectorParameter",
  "category": "Parameters",
  "position": { "x": -200.0, "y": 110.0 },
  "properties": {
    "parameterName": "BaseTint",
    "value": [0.18, 0.82, 0.9, 1.0]
  },
  "pins": [ ... ]
}
```

| 字段 | 必填 | 说明 |
|---|---|---|
| `id` | 是 | 无符号正整数，文档内唯一。用于连线引用。 |
| `type` | 是 | 节点简名，必须存在于 `NodeRegistry`。 |
| `ueClass` | 否 | UE5 的 MaterialExpression 类名；导入器优先使用它，缺失时按 `type` 推导。MaterialOutput 的该字段为 `Material`。 |
| `category` | 否 | 便于 UI 阅读的分类。加载器从注册表重新取值，不依赖此字段。 |
| `position` | 否 | 节点画布坐标 `{x, y}`。缺省 `(0,0)`。 |
| `properties` | 是（可为 `{}`） | 由节点定义决定字段集合；不允许未知键。 |
| `pins` | 是 | 所有输入和输出引脚。加载器按 `name` + `direction` 与注册表匹配。 |

### properties 值编码

| 属性 kind | JSON 表示 | 例子 |
|---|---|---|
| Float | number | `0.5` |
| Vector2 | `[x, y]` | `[0.05, 0.02]` |
| Vector3 | `[x, y, z]` | `[0.2, 0.8, 1.0]` |
| Vector4 | `[x, y, z, w]` | `[0.2, 0.8, 1.0, 1.0]` |
| Color3 / Color4 | 同 Vector3 / Vector4 | 颜色以 RGBA 数组导出 |
| Bool | boolean | `true` |
| String | string | `"Roughness"` |
| AssetPath | string | `"assets/textures/T_HoloGrid.png"` |

- 文本属性使用 UTF-8。
- `AssetPath` 可以是相对于 MaterialForge 根目录的路径，也可以是绝对路径。
  应用内的纹理选择器会尽量把项目内路径转为相对路径以便分发。

### pins[]

```json
{
  "id": 1022,
  "name": "Value",
  "direction": "output",
  "type": "Vector4",
  "resolvedType": "Vector4"
}
```

| 字段 | 说明 |
|---|---|
| `id` | 文档内引脚 ID，唯一；连线引用它。 |
| `name` | 与 NodeDefinition 的引脚名相匹配；同时也用于 UE 导入的输入名 fallback。 |
| `direction` | `"input"` / `"output"`。 |
| `type` | 声明类型：`Float` / `Vector2` / `Vector3` / `Vector4` / `Texture` / `Sampler` / `Wildcard`。 |
| `resolvedType` | 仅当 `type` 为 `Wildcard` 时出现，表示 TypeResolver 推导后的有效类型。导入器不依赖它。 |
| `default` | 仅输入且非 Texture/Sampler 时出现，未连线时的默认值。 |

导出的 pin 顺序固定为所有输入（按定义顺序）后所有输出。加载器要求文件包含
每个声明的引脚，且不能重复；这能避免旧版本文件静默错连。

## links[]

```json
{
  "id": 100005,
  "fromNode": 7,
  "fromPin": 1022,
  "fromPinName": "Value",
  "toNode": 8,
  "toPin": 1024,
  "toPinName": "B"
}
```

| 字段 | 说明 |
|---|---|
| `id` | 连线 ID，图内唯一。 |
| `fromNode` / `fromPin` | 源节点、源输出引脚。 |
| `fromPinName` | 源输出引脚名；便于人工阅读和脚本容错。 |
| `toNode` / `toPin` | 目标节点、目标输入引脚。 |
| `toPinName` | 目标输入引脚名。 |

规则：

- 每条边必须从 `output` 指向 `input`；
- 一个输入最多一条边；
- 加载器会使用与编辑器相同的 `CanConnect` 校验并拒绝环、类型不匹配、重复
  输入连接；
- `id` 重新导入后可能被重新分配，但按节点/引脚名与结构恢复的连线关系保持不变。

## 原子加载与错误

`GraphSerializer::FromJson` 首先在临时 `MaterialGraph` 上重建；只有全部节点、
属性、引脚与连线通过校验后才 `move` 到调用方的图。加载失败时当前文档保持不变，
错误文本包含具体节点、属性或引脚名。

校验包括：

- schema / version；
- 节点 type 存在；
- 节点/引脚 ID 唯一且非 0；
- 属性键和值类型合法；
- 重新序列化所需的每个声明引脚都存在；
- 每条连线端点存在、方向正确、不重复占用输入；
- 类型和环检查；
- 恰好一个 MaterialOutput。

## UE5 导入

`tools/ue5/import_material_graph.py` 直接使用本文件。额外的贴图映射文件是
独立 JSON：

```json
{
  "assets/textures/T_HoloGrid.png": "/Game/MaterialForge/Textures/T_HoloGrid"
}
```

贴图映射不作为材质图 schema 的一部分，因为它依赖具体的 UE 项目内容布局。

## Versioning

- 任何影响旧文件可加载性的改动都必须提升 `version`，并在加载器中显式迁移或
  拒绝。
- 新增可选属性、忽略未知顶层字段可以保持兼容；但当前加载器刻意对未知节点属性
  报错，以避免参数丢失。
- 若需要稳定引用，请把外部自动化建立在 `type` + 节点/引脚名之上，而不是绝对
  数字 ID。
