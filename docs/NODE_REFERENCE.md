# UE5 材质节点参考

本表由 `src/core/NodeRegistry.cpp` 中的实际定义整理，共 91 个节点定义，
其中 87 个可在材质节点库中直接添加；另含 `MaterialOutput`、函数图专用的
`FunctionInput` / `FunctionOutput`，以及按函数资产动态生成引脚的
`MaterialFunctionCall`。`Comment` 是编辑器注释框，不参与着色器求值。
`ueClass` 是 JSON 中的 `ueClass` 字段，也是 UE5 Python 导入时使用的类名。

## Material Settings

材质级设置保存在图 JSON 的 `material` 对象中：`domain`、`blendMode`、
`shadingModel`、`translucencyLightingMode`、`twoSided`、
`ditheredLodTransition`、`castDynamicShadowAsMasked`、
`useEmissiveForStaticLighting`、`opacityMaskClip`。Inspector 的
**MATERIAL SETTINGS** 标签可直接编辑这些值；修改 blend/shading/mask 设置会
重新生成预览 shader。

其中 `FunctionInput` / `FunctionOutput` 仅用于 `.mffunction` 蓝图函数图，
不会出现在材质节点库中；`MaterialFunctionCall` 是材质图中的函数调用节点，
其引脚由被引用函数动态生成，也不在节点库中直接列出。

所有未连接的数值输入都可以直接在节点上（Math 类的 Float 输入）或右侧
Inspector 的 **INPUT DEFAULTS** 区域编辑；这些常量会写入材质 JSON 的
`pins[].default` 并在重新打开时恢复。

以下记号：

- **W** = `Wildcard`，由上游连接动态推导为 Float / Vector2 / Vector3 / Vector4；
- 输入未连接时使用定义中的默认值；
- “主输出”指单输出节点默认的空输出名，UE Python 中用 `""` 连接；
- 预览语义是 MaterialForge 的 GLSL 实现，不保证与 UE 着色器逐位一致。

## Function（仅蓝图函数图）

| Node type | UE class | Properties | Pins | 说明 |
|---|---|---|---|---|
| `FunctionInput` | `MaterialExpressionFunctionInput` | `inputName` String, `pinType` Float/Vector2/Vector3/Vector4 | Value: 对应类型 | 函数被调用时暴露的强类型输入；名称和类型可在 Inspector 修改。 |
| `FunctionOutput` | `MaterialExpressionFunctionOutput` | `outputName` String, `pinType` Float/Vector2/Vector3/Vector4 | Value: 对应类型 | 函数返回的强类型结果；名称和类型可在 Inspector 修改。 |

函数图使用与材质相同的图 JSON 序列化，但不需要（也不应包含）MaterialOutput。
函数可以从节点库的 **MATERIAL FUNCTIONS** 区域双击打开，也可以拖入材质图创建
`MaterialFunctionCall`；用户工程中的 `.mffunction` 会自动出现在该区域。

## Function Call（材质图）

| Node type | UE class | Properties | Pins | 说明 |
|---|---|---|---|---|
| `MaterialFunctionCall` | `MaterialExpressionMaterialFunctionCall` | `functionPath` String, `functionName` String | 由函数内的 `FunctionInput` / `FunctionOutput` 动态生成 | 选中节点后 Inspector 提供 **Open & Reveal Material Function**，打开函数分页并在内容浏览器定位资源。预览生成时会把函数图内联展开；引脚默认值写入 JSON。 |

**Collapse to Function**：材质图中多选节点后执行 `Edit → Collapse to
Function...`，会生成 `functions/MF_Collapsed_N.mffunction`。选区边界上的
外部输入变成 `FunctionInput`，外部输出变成 `FunctionOutput`，内部连线原样
复制；原图用一个 `MaterialFunctionCall` 替换并恢复外部连线。函数引脚类型
来自边界引脚的有效类型（Float/Vector2/Vector3/Vector4）。

## Output

| Node type | UE class | Inputs | Outputs | 预览 / 导入说明 |
|---|---|---|---|---|
| `MaterialOutput` | Material（根节点，不属于 MaterialExpression） | Base Color, Metallic, Specular, Roughness, Emissive Color, Normal, Ambient Occlusion, Opacity, Opacity Mask, World Position Offset, Pixel Depth Offset, Refraction, Clear Coat, Clear Coat Roughness, Subsurface Color, Anisotropy | — | 导入时用 `connect_material_property` 连接到 `MP_BASE_COLOR`、`MP_OPACITY_MASK`、`MP_WORLD_POSITION_OFFSET`、`MP_CLEAR_COAT` 等材质属性。混合模式、着色模型、双面、遮罩阈值等由材质设置导出。 |

## Constants

| Node type | UE class | Properties | Outputs | 说明 |
|---|---|---|---|---|
| `Constant` | `MaterialExpressionConstant` | `value` Float (0.5) | Value: Float | 导出为 `r`。 |
| `Constant2Vector` | `MaterialExpressionConstant2Vector` | `value` Vector2 | Value: Vector2 | 导出为 `r` / `g`。 |
| `Constant3Vector` | `MaterialExpressionConstant3Vector` | `value` Color3 | Value: Vector3 | 导出为 `constant` (`LinearColor`)。 |
| `Constant4Vector` | `MaterialExpressionConstant4Vector` | `value` Color4 | Value: Vector4 | 导出为 `constant` (`LinearColor`)。 |

## Parameters

| Node type | UE class | Properties | Outputs | 说明 |
|---|---|---|---|---|
| `ScalarParameter` | `MaterialExpressionScalarParameter` | `parameterName`, `value`, `parameterGroup`, `sortPriority`, `description`, `sliderMin`, `sliderMax` | Value: Float | 预览中生成 `uniform float uParam_<name>_<id>`；导入为 `parameter_name` + `default_value`。Material Instance 使用元数据生成分组滑块。 |
| `VectorParameter` | `MaterialExpressionVectorParameter` | `parameterName`, `value`, `parameterGroup`, `sortPriority`, `description` | Value: Vector4 | 预览中生成 `uniform vec4 ...`；导入为 `parameter_name` + `default_value` (`LinearColor`)。 |

材质参数名在 UE 中需要唯一；生成器检测到重名会在 GLSL 诊断中给出 warning。

Material Instance（`.mfinstance`）保存 `parentMaterial` 和稀疏参数覆盖。实例只
修改覆盖值，不会改写父材质；参数分组、排序、描述和滑块范围来自父材质参数节点
的元数据。
参数值改动只更新 uniform，不触发预览 shader 重编译。

## Math

### 二元

所有 A/B 均为 `Wildcard`，输出为两者提升后的共同类型；预览使用 GLSL 内建。

| Node type | UE class | GLSL (preview) | 默认 A/B |
|---|---|---|---|
| `Add` | `MaterialExpressionAdd` | `a + b` | 0, 0 |
| `Subtract` | `MaterialExpressionSubtract` | `a - b` | 0, 0 |
| `Multiply` | `MaterialExpressionMultiply` | `a * b` | 1, 1 |
| `Divide` | `MaterialExpressionDivide` | `a / b` | 1, 1 |
| `Power` | `MaterialExpressionPower` | `pow(a, b)` | 1, 2 |
| `Min` | `MaterialExpressionMin` | `min(a, b)` | 0, 1 |
| `Max` | `MaterialExpressionMax` | `max(a, b)` | 0, 1 |
| `Fmod` | `MaterialExpressionFmod` | `mod(a, b)` | 1, 1 |

### 一元

所有 Input/Result 均为 `Wildcard`，输出类型跟随输入。

| Node type | UE class | GLSL (preview) |
|---|---|---|
| `Abs` | `MaterialExpressionAbs` | `abs(x)` |
| `Ceil` | `MaterialExpressionCeil` | `ceil(x)` |
| `Floor` | `MaterialExpressionFloor` | `floor(x)` |
| `Frac` | `MaterialExpressionFrac` | `fract(x)` |
| `Round` | `MaterialExpressionRound` | `round(x)` |
| `Sqrt` | `MaterialExpressionSqrt` | `sqrt(max(x, 0))` |
| `OneMinus` | `MaterialExpressionOneMinus` | `1 - x` |
| `Sine` | `MaterialExpressionSine` | `sin(x)` |
| `Cosine` | `MaterialExpressionCosine` | `cos(x)` |
| `Tangent` | `MaterialExpressionTangent` | `tan(x)` |
| `Saturate` | `MaterialExpressionSaturate` | `clamp(x, 0, 1)` |
| `Sign` | `MaterialExpressionSign` | `sign(x)` |
| `Truncate` | `MaterialExpressionTruncate` | `trunc(x)` |
| `Logarithm2` | `MaterialExpressionLogarithm2` | `log2(max(x, 1e-6))` |
| `Arcsine` | `MaterialExpressionArcsine` | `asin(clamp(x, -1, 1))` |
| `Arccosine` | `MaterialExpressionArccosine` | `acos(clamp(x, -1, 1))` |
| `Arctangent` | `MaterialExpressionArctangent` | `atan(x)` |

### 条件 / 比较 / 向量

| Node type | UE class | Inputs / properties | Output | 说明 |
|---|---|---|---|---|
| `If` | `MaterialExpressionIf` | A (Float), B (Float), `A > B` (W), `A == B` (W), `A < B` (W) | Result: W | UE `If` 的三分支语义；三个分支值共同提升类型。 |
| `Switch` | `MaterialExpressionSwitch` | Pick (Float), A (W), B (W) | Result: W | `Pick > 0.5` 时选择 A，否则 B。 |
| `Greater` / `GreaterEqual` / `Less` / `LessEqual` / `Equal` / `NotEqual` | 对应 UE 表达式 | A (Float), B (Float) | Result: Float | 返回 1 或 0；`Equal` 使用 1e-4 容差。 |
| `AppendVector` | `MaterialExpressionAppendVector` | A (W), B (W) | Result: W | 组件数相加，最多 4 个组件。 |
| `ComponentMask` | `MaterialExpressionComponentMask` | Input (W); 属性 `r` / `g` / `b` / `a` Bool | Result: W | 按勾选通道输出 Float / Vector2 / Vector3 / Vector4。 |
| `BreakVector2` / `BreakVector3` / `BreakVector4` | `MaterialExpressionBreakVector*` | Input (Vector2/3/4) | R/G/B/A: Float | 拆分向量分量。 |
| `Length` | `MaterialExpressionLength` | Input (W) | Result: Float | 标量使用 `abs`，向量使用 `length`。 |
| `Distance` | `MaterialExpressionDistance` | A (W), B (W) | Result: Float | 标量使用 `abs(a-b)`，向量使用 `distance`。 |
| `SmoothStep` | `MaterialExpressionSmoothStep` | Value (W), Min (W), Max (W) | Result: W | `smoothstep(min, max, value)`。 |
| `Step` | `MaterialExpressionStep` | Value (W), Edge (W) | Result: W | `step(edge, value)`。 |
| `ConstantBiasScale` | `MaterialExpressionConstantBiasScale` | Input (W), Bias (W), Scale (W) | Result: W | `Input * Scale + Bias`。 |
| `RotateAboutAxis` | `MaterialExpressionRotateAboutAxis` | Position (Vector3), Axis (Vector3), Angle (Float) | Result: Vector3 | Rodrigues 旋转，Angle 单位为弧度。 |
| `SphereMask` | `MaterialExpressionSphereMask` | A/B (Vector3), Radius (Float), Hardness (Float) | Result: Float | 距离场球体遮罩。 |
| `Noise` | `MaterialExpressionNoise` | Position (Vector3) | Result: Float | 预览使用确定性的 3D value noise；属性 `scale`。 |

### 插值 / 向量

| Node type | UE class | Inputs | Output | 说明 |
|---|---|---|---|---|
| `Lerp` | `MaterialExpressionLerp` | A (W), B (W), Alpha (Float) | Result: W | `mix(A, B, Alpha)`；A/B 类型提升。 |
| `Clamp` | `MaterialExpressionClamp` | Value (W), Min (W), Max (W) | Result: W | `clamp(value, min, max)`；三者共同提升类型。 |
| `DotProduct` | `MaterialExpressionDotProduct` | A (W), B (W) | Result: Float | `dot(a, b)`；标量输入退化为乘法。 |
| `CrossProduct` | `MaterialExpressionCrossProduct` | A (Vector3), B (Vector3) | Result: Vector3 | `cross(a, b)`。 |
| `Normalize` | `MaterialExpressionNormalize` | Input (W) | Result: W | Float 输入时返回符号值；其他类型使用 `normalize`。 |

## Utility

| Node type | UE class | Inputs / properties | Outputs | 说明 |
|---|---|---|---|---|
| `Desaturation` | `MaterialExpressionDesaturation` | Color (Vector3), Fraction (Float) | Result: Vector3 | 按亮度 `(0.3,0.59,0.11)` 插值。 |
| `Fresnel` | `MaterialExpressionFresnel` | 属性 `exponent` Float, `baseReflectFraction` Float | Result: Float | UE 中 Exponent 是节点属性而不是输入引脚；预览使用几何法线近似。导入设置 `exponent` 和 `base_reflect_fraction`。 |
| `StaticSwitchParameter` | `MaterialExpressionStaticSwitchParameter` | True (W), False (W); 属性 `value` Bool | Result: W | 编译期开关；预览按属性选择分支并导出为静态开关参数。 |
| `TwoSidedSign` | `MaterialExpressionTwoSidedSign` | — | Sign: Float | 预览始终为 1。 |
| `Reroute` | `MaterialExpressionReroute` | Input (W) | Output: W | 透传节点；用于整理连线，生成的是同类型局部变量。 |
| `Comment` | — | 属性 `text` String, `color` Color3; 节点尺寸 | — | 可调整大小的注释框；`Group Selected` 会围绕所选节点创建一个注释组。 |

## Coordinates

| Node type | UE class | Inputs | Outputs | Properties / 说明 |
|---|---|---|---|---|
| `TextureCoordinate` | `MaterialExpressionTextureCoordinate` | — | UV: Vector2 | `uTiling`, `vTiling`；预览为 `vUV * vec2(u, v)`。导入 `u_tiling` / `v_tiling`。 |
| `Panner` | `MaterialExpressionPanner` | Coordinate (Vector2), Time (Float) | UV: Vector2 | 属性 `speed` Vector2；未连接 Time 时使用全局 `uTime`。导入 `speed_x` / `speed_y`。 |
| `Rotator` | `MaterialExpressionRotator` | Coordinate (Vector2) | UV: Vector2 | 属性 `center` Vector2、`speed` Float；预览使用全局时间。导入 `center_x` / `center_y` / `speed`。 |
| `BumpOffset` | `MaterialExpressionBumpOffset` | Coordinate (Vector2), Height (Float), HeightRatio (Float), ReferencePlane (Float) | UV: Vector2 | 预览使用简单的 UV 视差近似。 |
| `Time` | `MaterialExpressionTime` | — | Time: Float | 全局 `uTime`。 |
| `WorldPosition` | `MaterialExpressionWorldPosition` | — | World Position: Vector3 | 像素世界坐标。 |
| `VertexNormal` | `MaterialExpressionVertexNormal` | — | Vertex Normal: Vector3 | 世界空间插值法线的归一化值。 |
| `CameraVector` | `MaterialExpressionCameraVector` | — | Camera Vector: Vector3 | 像素到相机的归一化向量。 |
| `CameraPosition` | `MaterialExpressionCameraPositionWS` | — | Camera Position: Vector3 | 预览相机世界坐标 `uCameraPos`。 |
| `ObjectPosition` | `MaterialExpressionObjectPositionWS` | — | Object Position: Vector3 | 预览为原点。 |
| `ActorPosition` | `MaterialExpressionActorPositionWS` | — | Actor Position: Vector3 | 预览为原点。 |
| `LightVector` | `MaterialExpressionLightVector` | — | Light Vector: Vector3 | 指向预览主光的方向。 |
| `ReflectionVector` | `MaterialExpressionReflectionVectorWS` | — | Reflection Vector: Vector3 | `reflect(-V, N)`。 |
| `ScreenPosition` | `MaterialExpressionScreenPosition` | — | Screen Position: Vector2 | 预览为视口 UV。 |
| `ViewSize` | `MaterialExpressionViewSize` | — | View Size: Vector2 | 预览视口像素尺寸 `uViewSize`。 |
| `VertexColor` | `MaterialExpressionVertexColor` | — | Vertex Color: Vector4 | 预览网格无顶点色，返回白色。 |
| `PixelDepth` | `MaterialExpressionPixelDepth` | — | Depth: Float | 相机到像素的距离。 |
| `SceneDepth` | `MaterialExpressionSceneDepth` | — | Depth: Float | 预览近似为相机距离。 |

## Textures

| Node type | UE class | Inputs | Outputs | Properties / 说明 |
|---|---|---|---|---|
| `Sampler` | `MaterialExpressionSampler` | — | Sampler: Sampler | 共享采样设置：`filter`、`addressU/V`、`srgb`、`mipBias`、`maxAnisotropy`、`textureGroup`。连接到 TextureSample 的 Sampler 输入时覆盖纹理自身设置。 |
| `TextureObject` | `MaterialExpressionTextureObject` | — | Texture: Texture | `texture` AssetPath 以及 sampler 设置。预览加载图片并按 sRGB/filter/wrap 配置 GL 纹理；JSON 导入时解析成 TextureSample 的 `texture` 属性。 |
| `TextureObjectParameter` | `MaterialExpressionTextureObjectParameter` | — | Texture: Texture | 命名纹理参数，属性 `parameterName` / `texture` 及 sampler 设置；导入为 TextureObjectParameter。 |
| `TextureSample` | `MaterialExpressionTextureSample` | UVs (Vector2), Texture (Texture), Sampler (Sampler) | RGBA: Vector4, RGB: Vector3, R/G/B/A: Float | UVs 未连接时使用 `vUV`；Texture 未连接时使用 fallback 棋盘纹理。Sampler 连接时覆盖纹理采样设置。 |
| `TextureSampleParameter2D` | `MaterialExpressionTextureSampleParameter2D` | UVs (Vector2), Texture (Texture), Sampler (Sampler); 属性 `parameterName` / `texture` + sampler 设置 | RGBA/RGB/R/G/B/A | 带参数名的纹理采样；Texture 未连接时使用 `texture` 属性，否则使用回退纹理。 |
| `SceneTexture` | `MaterialExpressionSceneTexture` | UVs (Vector2); 属性 `sceneTextureId` String | Color: Vector4, Size: Vector2, InvSize: Vector2 | 仅提供节点定义和 Size/InvSize 输出；由于本版本不做场景联动，Color 使用 UV 渐变占位色，不读取实际场景。 |

OpenGL 预览会在纹理绑定时应用 filter / wrap / LOD bias / anisotropy 和 sRGB
格式；DX12 / Vulkan 计算预览目前共用一个 sampler state，纹理节点仍保留这些设置
并写入 JSON，后续可扩展为逐纹理 sampler 描述符。

## 引脚类型与颜色

| Type | 颜色用途 | 组件数 | 转换规则 |
|---|---|---|---|
| `Float` | 浅青灰 | 1 | 可广播到 VectorN。 |
| `Vector2` | 绿 | 2 | 截断/补维。 |
| `Vector3` | 琥珀 | 3 | 截断/补维。 |
| `Vector4` | 粉红 | 4 | 截断/补 alpha 1。 |
| `Texture` | 橙色 | — | 只能接 Texture。 |
| `Sampler` | 紫色 | — | 为 Schema 保留；当前节点库没有独立 sampler 节点。 |
| `Wildcard` | 灰 | — | 仅内部多态数学节点使用，显示时会解析为具体类型。 |

Vector2/3/4 输出引脚支持右键 **拆分引脚**：生成 R/G/B/A（按组件数）Float 输出，
并隐藏原向量引脚；拆分通道在 JSON 中用 `splitFrom` / `splitIndex` 记录。组件引脚
或节点右键菜单可以 **合并引脚** / **合并拆分引脚**，恢复原输出。

## 预览与 UE 的语义差异

| 主题 | MaterialForge 预览 | UE5 导入后 |
|---|---|---|
| 光照 | 固定的 studio PBR 近似 | 材质所在的场景光照 |
| Fresnel | 几何法线 + 视线方向的近似 | UE `MaterialExpressionFresnel` 内部实现 |
| Panner / Rotator 时间 | 预览全局 uTime | 材质所在世界的时间 |
| Normal 输入 | 切线空间，TBN 重建 | UE 材质法线语义 |
| 纹理色彩空间 | 统一按 sRGB 加载 | 由 UE Texture 资产的 sRGB 设置决定 |
| Opacity / 混合 | Opacity 输入参与 alpha 混合 | 由材质 Blend Mode 决定；Masked / Translucent 等预览仍待接入 |
| VectorParameter 输出 | 单个 Vector4 | UE 可额外提供通道输出；导入后仍可手动拆分 |
