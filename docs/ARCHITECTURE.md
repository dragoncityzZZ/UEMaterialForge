# 架构与实现说明

本文记录 MaterialForge 的模块边界、关键数据结构、代码生成流程和扩展方式。
面向想继续维护或移植该编辑器的开发者。

## 1. 设计目标

1. **数据与 UI 解耦**：`material_core` 不依赖 ImGui / OpenGL，可独立做单元测试，
   也方便未来做命令行工具或其他前端。
2. **UE 节点语义优先**：节点类型、引脚名、属性命名尽量与 UE5 材质表达式一致；
   导出的 JSON 直接携带 `ueClass`。
3. **生成代码即预览**：预览渲染不是手写固定 shader，而是每次图变化后重新生成
   fragment shader，所见即所得。
4. **可诊断**：类型错误、悬空连线、环、shader 编译错误都通过 `GeneratedShader`
   或渲染器日志暴露给 UI，而不是静默失败。
5. **低依赖**：数学层自包含，资源管线仅使用 stb_image；没有引入 GLM、spdlog、
   ImGui 测试引擎或运行时代码生成库。

## 2. 分层

```text
┌────────────────────────────────────────────────────────────┐
│ app       Application / NodeGraphView / UiWidgets / FileBrowser │
├────────────────────────────────────────────────────────────┤
│ project   MaterialProject / RecentProjects (.mfproject)     │
├────────────────────────────────────────────────────────────┤
│ render    PreviewRenderer facade / OpenGL / DX12 / Vulkan backends / HLSL │
├────────────────────────────────────────────────────────────┤
│ codegen   GlslGenerator                                     │
│ core      Node / Pin / Graph / NodeRegistry / TypeResolver  │
│ io        GraphSerializer (JSON)                             │
└────────────────────────────────────────────────────────────┘
```

依赖方向只能是自上而下。`material_core`（core、codegen、io）不 include OpenGL 或
ImGui 头文件；`material_render` 依赖 core，拥有 OpenGL / D3D12 / Vulkan 对象
生命周期以及 HLSL 预览着色器翻译；`MaterialForge` 可执行文件负责 UI 与平台窗口。

## 3. 数据模型

### 3.1 Node / Pin / Link

```cpp
enum class PinType : uint8_t {
    Float, Vector2, Vector3, Vector4, Texture, Sampler, Wildcard
};

struct Pin {
    PinId id;
    NodeId nodeId;
    std::string name;
    PinDirection direction;
    PinType type;                 // 声明类型，数学节点为 Wildcard
    PropertyValue defaultValue;   // 仅输入未连接时使用
    std::string tooltip;
};

struct Node {
    NodeId id;
    std::string type;                 // "Constant3Vector", "TextureSample", ...
    Vec2 position;
    std::vector<Pin> inputs, outputs;
    std::map<std::string, PropertyValue> properties;
};

struct Link {
    LinkId id;
    PinId fromPin;   // output
    PinId toPin;     // input
};
```

- 节点内存由 `MaterialGraph` 以 `unique_ptr` 管理，因此编辑时指针稳定；
  连线只存 PinId，不存裸指针。
- 每个输入引脚最多一条入边。`MaterialGraph::Connect` 会先断开该输入原有的
  连线（这与 UE 材质输入的行为一致）。
- 每个输出可以 fan-out 到多个输入。
- 节点和引脚 ID 由图内单调计数器生成；不依赖内存地址。重新加载 JSON 时会
  重建计数器，并把文件里的旧 ID 映射到新 ID 以恢复连线。
- `NodeDefinition` 是静态元数据（注册表），`Node` 是运行期实例。这样节点库、
  Inspector、JSON 和代码生成共享同一份定义。
- `MaterialFunctionCall` 没有静态引脚：从内容浏览器或节点库拖入函数时，
  `NodeGraphView` 读取 `.mffunction`，按其中 `FunctionInput` / `FunctionOutput`
  节点动态创建输入/输出 `Pin`。JSON 加载时会从 `pins` 数组重建这些动态引脚，
  再按引脚名恢复连线。
- 未连接数值输入的 `Pin::defaultValue` 可在节点画布（Math 的 Float 输入）和
  Inspector 的 **INPUT DEFAULTS** 区域编辑；`GraphSerializer` 写入
  `pins[].default`，加载时按引脚类型恢复。

### 3.2 图的不变量

`MaterialGraph` 在以下位置维护不变量：

- `AddNode`：按注册表创建声明引脚和默认属性；输出节点并非自动唯一，但
  `AddNodeByType` 拒绝重复的 MaterialOutput，导入器也会校验恰好一个输出。
- `CanConnect`：
  - 两端引脚必须存在且方向正确；
  - 非数值类型（Texture / Sampler）必须完全同类型；
  - 数值类型均可连接，实际转换交给代码生成；
  - 不允许自连接；
  - DFS 检测下游能否到达源节点，避免环。
- `RemoveNode`：同时移除所有相关 Link。
- `RemoveLink` / `DisconnectInput`：更新图并 bump revision。

### 3.3 Revision 和脏标记

```cpp
ContentRevision()   // 连线 / 节点 / 属性 / 材质名变化
LayoutRevision()    // 只有节点位置变化
IsDirty()           // 任一种变化都会让文档变脏
```

`Application` 只在 `ContentRevision` 变化时重新生成 GLSL；拖动节点不会导致
shader 重编译。`MarkSaved()` 只清脏标记，不清 revision；文件保存不会触发预览
重建。

## 4. 节点注册表

`NodeRegistry` 是单例，在构造时创建全部节点定义：

```cpp
struct NodeDefinition {
    std::string type;             // UE class 后缀
    std::string displayName;
    std::string category;
    std::string description;
    std::string ueClass;          // MaterialExpressionXxx
    std::vector<PinDefinition> inputs, outputs;
    std::vector<PropertySpec> properties;
    PinTypeRule typeRule;         // Fixed / PromoteInputs / MatchFirstInput / AppendInputs
    bool isMaterialOutput;
};
```

`PinTypeRule` 只影响 Wildcard 类型推导，不参与代码生成的具体公式。代码生成
仍按实际输入表达式类型做最终转换，两边对同一图会得到一致的类型。

新增节点的步骤：

1. 在 `NodeRegistry.cpp` 中添加定义（或 helper 工厂）；
2. 在 `GlslGenerator::CompileNode` 添加该类型的 GLSL 语义；
3. 若 UE 有对应类，填写正确的 `ueClass`；
4. 在 `tools/ue5/import_material_graph.py::apply_node_properties` 添加属性映射
   （如果该节点有非默认属性）；
5. 在 `tests/test_core.cpp` 与 `test_shader_compile.cpp` 的遍历测试里自动覆盖
   新节点（它们遍历注册表全部定义）。

## 5. 类型推导

数学节点为了匹配 UE 的多态行为，输入输出使用 `Wildcard`。`TypeResolver` 通过
递归 + 缓存 + visiting 集合计算实际类型：

```text
GetOutputType(pin):
  if pin.type != Wildcard: return pin.type

  rule = nodeDefinition.typeRule
  Fixed             -> Float
  PromoteInputs     -> promote(每个输入的 effective type)
  MatchFirstInput   -> 第一个数值输入的类型
  AppendInputs      -> 输入通道数求和后的 VectorN

GetInputType(pin):
  if linked: return GetOutputType(sourcePin)
  else:      return 类型对应的默认值类型（通常是 Float）
```

类型提升规则见 `PromotePinTypes`。实际 GLSL 转换由 `Coerce` 完成：

- `Float → VectorN`：广播；
- `VectorA → VectorB`：
  - A > B：取前 B 个分量；
  - A < B：补 0（3→4 补 alpha 1）；
- `VectorN → Float`：取 `.x`。

这一转换在表达式消费处显式生成，避免依赖 GLSL 隐式标量广播的可读性问题。

## 6. GLSL 代码生成

`GlslGenerator::Generate` 的结果：

```cpp
struct GeneratedShader {
    bool success;
    std::string fragmentSource;
    std::string diagnostic;
    std::vector<ShaderParameterBinding> parameters;
    std::vector<ShaderTextureBinding> textures;
    std::vector<std::string> warnings;
    int evaluatedNodeCount;
};
```

### 6.1 递归编译与缓存

`Compiler::EvaluateOutput(pin)` 从输出引脚向上递归：

- `m_NodeCache` 保证一个节点只编译一次；
- `m_Compiling` 检测环，给出精确错误；
- 每个节点返回一个 `NodeEvaluation`，其中按输出引脚名保存 `Expr { code, type }`；
- 子节点先完成，因此生成的语句天然满足依赖顺序；
- 多输出节点只需一个实现：`TextureSample` 生成一个 `vec4` 局部变量，再把
  `RGB`、`R`、`G`、`B`、`A`、`RGBA` 映射为它的 swizzle 表达式。

典型生成的语句：

```glsl
// Multiply #8
vec4 mf_Multiply_8 = (vec4(mf_TextureSample_6.rgb, 1.0) * uParam_BaseTint_7);
```

这里 RGB 先被提升到 Vector4，VectorParameter 通过 uniform 参与计算，因此拖动
参数颜色时无需重编译 shader。

### 6.2 节点实现

每个节点在 `CompileNode` 中有明确的 GLSL 映射：

- 二元数学节点：先 `PromotePinTypes`，再 `Coerce` 到共同类型；
- 一元节点：类型跟随输入，按类型构造函数生成常量（如 `vec3(1.0) - x`）；
- `Lerp` → `mix`；`Clamp` → `clamp`；`Frac` → `fract`；
- `DotProduct`、`CrossProduct`、`Normalize` 使用 GLSL 内建；
- `Fresnel` 使用几何法线 `mf_N` 与视线 `mf_V` 在 main 前部计算；
- `Panner` 未连接 Time 时使用全局 `uTime`；
- 参数节点生成 uniform，不把默认值烘进 shader，因此拖动参数可以即时预览；
- `TextureObject` 生成 `uniform sampler2D uTexture_<nodeId>` 并收集纹理 binding；
- `TextureSample` 默认 UV 为 `vUV`，默认纹理是棋盘 fallback。

未连接输入使用引脚 `defaultValue` 加该输入的有效类型生成字面量，从而支持
“只连一个输入的 Add”等 UE 常见场景。节点画布和 Inspector 都可以修改这些值，
`GraphSerializer` 会在加载时恢复。

### 6.3 材质函数调用

`GlslGenerator::Generate(graph, glslVersion, baseDirectory)` 的第三个参数是
相对函数路径的解析基准（工程根目录或文档所在目录）。

`Compiler::CompileFunctionCall` 对 `MaterialFunctionCall`：

1. 解析并加载 `.mffunction`；递归栈 `m_FunctionStack` 检测循环调用；
2. 把调用节点的输入表达式绑定到函数内的 `FunctionInput` 名称；
3. 为函数图创建子 `Compiler`，变量名和 uniform 名加上调用节点前缀，避免与
   调用者图中的节点 ID 冲突；
4. 逐个 `FunctionOutput` 求值，把子编译器生成的函数体语句追加到父 shader；
5. 合并子编译器的参数、纹理和 warning。

函数输入/输出目前为标量 Float；动态引脚名、默认值和类型都写入材质 JSON，
因此重新打开材质时不需要重新解析函数资产也能恢复连线。

### 6.4 材质输出与预览光照

`MaterialOutput` 的八个输入分别被强制转换后接入：

```glsl
vec3  mf_baseColor;
float mf_metallic, mf_specular, mf_roughness, mf_ao, mf_opacity;
vec3  mf_emissive;
```

若 Normal 输入已连接，则把值视为切线空间法线，使用
`TBN = (vTangent, cross(N, T), N)` 扰动几何法线。随后使用简化
Cook-Torrance + GGX 三点 studio 光照和环境近似，最后 tone mapping 与 gamma
写入 `FragColor`。

> 预览着色刻意保持轻量，目的是让图编辑反馈快速、稳定；它不是 UE 着色器的
> 逐行移植。需要严格一致时应以 UE 导入后的结果为准。

## 7. 实时预览与 GPU 后端

`PreviewRenderer` 是后端门面，持有 `OpenGLPreviewRenderer`、
`D3D12PreviewRenderer` 或 `VulkanPreviewRenderer`。三条管线都输出一个 OpenGL
纹理名，ImGui 编辑器外壳只负责合成，因此切换后端不会影响 UI 代码。

- **OpenGL**：编译背景 shader、创建球体/平面 VBO、fallback 棋盘纹理；
  `SetMaterial` 比较 fragment source 决定是否重新编译；每帧按预览面板尺寸
  重建 / 复用 4x MSAA FBO，绘制背景与网格，blit 到 RGBA8 resolve texture。
- **DirectX 12**：`HlslPreviewShaderGenerator` 把同一张材质 DAG 的 GLSL
  逐行翻译为 HLSL 计算着色器（射线求交球体/平面、材质表达式、PBR 求值），
  `D3DCompiler` 编译为 DXBC，D3D12 计算管线写入 R8G8B8A8 UAV，回读后上传到
  OpenGL 纹理。
- **Vulkan**：同一份 HLSL 由随附 `glslang.exe` 编译为 SPIR-V，Vulkan 计算
  管线写入 storage image，回读后上传到 OpenGL 纹理。

偏好设置中的选择保存到 `settings.json`；任一后端初始化失败时回到 OpenGL，
并把原因写入 CMD 控制台。OpenGL 上下文优先请求 4.6，失败时回退 4.3 / 3.3，
GLSL 版本随上下文调整。

球体使用经纬细分（96×64）与切线生成；平面是单个 quad，法线 +Z、切线 +X。
OpenGL 后端使用 `vWorldPos/vNormal/vTangent/vUV` 顶点接口；DX12 / Vulkan
后端在计算着色器内解析同一相机和几何，因此无需顶点缓冲。

视口交互在 Application 层完成，不污染渲染器：拖拽 → `Orbit`，滚轮 → `Zoom`，
双击 → `Camera.Reset`。`PreviewRenderer` 还负责在材质 shader 编译失败时返回
false 并保留错误日志。

## 8. 控制台、偏好设置、撤销与工作区

- **撤销/重做**：每个 `EditorDocument` 保存独立的 `undoStack` / `redoStack`
  （完整 `MaterialGraph` 快照，最多 64 步）。帧末比较内容/布局 revision，
  在鼠标释放且无活动控件后把“变更前快照”入栈；拖拽和连续输入合并为一步。
  `Ctrl+Z` / `Ctrl+Y` / `Ctrl+Shift+Z` 和 Edit 菜单共用 `Undo()` / `Redo()`。
- **Buffer Visualization / 像素探针**：`PreviewBuffer` 枚举（`core/PreviewBuffer.h`）
  作为 `GlslGenerator::Generate` 的最后参数，生成带 `Buffer visualization`
  分支的调试片元着色器；切换缓冲只重编译预览 shader，不改变材质图。预览面板
  的 Pixel Probe 通过 `PreviewRenderer::ProbePixel` 读取共享 GL 颜色纹理
  （OpenGL resolve texture / DX12 / Vulkan 回传纹理），显示当前缓冲的 RGBA。
- **材质图组织**：`Comment` 是带 `text` / `color` 属性和持久化 `size` 的注释
  节点，`Group Selected` 以所选节点包围盒创建注释组；`Reroute` 是
  `MatchFirstInput` 的透传节点。对齐/分布、注释尺寸、分组和移动均通过
  `MaterialGraph::SetNodePosition` / `MarkLayoutChanged` 进入撤销历史；
  书签保存节点 ID 并通过 `NavigateToSelection` 聚焦。
- **Material Settings / Output**：`MaterialSettings`（domain、blendMode、
  shadingModel、twoSided、opacityMaskClip 等）随图 JSON 保存并参与
  shader 生成；`GeneratedShader::blendMode` 传给 HLSL 合成阶段，OpenGL
  预览按 blendMode 设置混合函数。Material Output 的 16 个输入在 GLSL 中
  统一求值；Opacity Mask 以二值 alpha 模拟（避免 compute shader 不支持
  discard），WPO/Refraction/Clear Coat/Subsurface 提供预览近似。
- **内容删除 / 强类型函数**：`MaterialProject::DeleteEntry` 统一删除文件或
  递归文件夹，并清理 manifest 资源、文件夹颜色、主材质引用；Application
  在删除前显示确认框，并关闭受影响的文档。`FunctionInput` / `FunctionOutput`
  的 `pinType` 决定函数签名，`MaterialFunctionCall` 的动态引脚继承该类型；
  `NodeGraphView::CollapseSelectedToFunction` 复制选区、生成边界函数 I/O，
  保存 `.mffunction` 后用调用节点替换原图。
- **Material Instance**：`MaterialParameters` 从父图收集 Scalar/Vector/Texture
  参数及分组、排序、描述、滑块元数据；`MaterialInstance` 保存 `.mfinstance`
  的父路径与稀疏覆盖。Application 用父图副本应用覆盖后生成预览，保存时只
  写实例文件；`DocumentKind::Instance` 使用独立标签页和实例参数面板。
- **纹理设置 / Sampler**：`SamplerSettings`（`core/TextureSettings.h`）从
  TextureObject / Sampler 节点读取 filter、wrap、sRGB、mip bias、anisotropy。
  `ShaderTextureBinding` 携带设置；OpenGL 在每次绑定纹理时应用对应 GL 参数，
  并按 sRGB 分键缓存上传。Sampler 节点连接到 TextureSample 的 Sampler 引脚时
  覆盖纹理自身设置；DX12/Vulkan 使用共享 sampler state。
- **预览场景**：`OpenGLPreviewRenderer` 支持 OBJ 自定义网格、HDRI 等距柱状
  背景、地面平面和全局曝光；`AppSettings` 持久化网格模式/路径、HDRI、地面和
  灯光强度。灯光参数传给 `GlslGenerator` 并写入 shader 常量，因此 OpenGL /
  DX12 / Vulkan 共用同一组 Key/Fill/Rim/Ambient/Exposure。OBJ 文件解析在
  `Mesh::CreateFromObj`，自动居中缩放并补算法线/切线。
- **UE5 桥接 / 缩略图**：`tools/ue5/push_material.py` 包装现有导入器，
  `pull_material_graph.py` 用 `MaterialEditingLibrary` 遍历表达式并通过
  `node_catalog.json`（`--export-node-catalog` 生成）映射回 MaterialForge
  节点、引脚和属性；`push_batch.py` / `pull_batch.py` 处理项目批量清单。
  `io/Ue5Clipboard` 则直接解析 UE 材质编辑器的 `Begin Object` 复制文本
  （UE4 与 UE5 紧凑格式），在画布 Ctrl+V 时重建节点、属性和内部连线。
  `app/McpServer` 在后台线程提供本地 MCP JSON-RPC/HTTP 服务，工具调用通过
  队列回到主线程执行；默认关闭，端口与开关保存在 `AppSettings` 中。
  `app/PluginManager` 扫描用户插件目录，按 `plugin.json` 加载 DLL，并通过
  `plugin/MaterialForgePlugin.h` 的 C ABI 注册控制台命令。内容浏览器的
  “添加包”从 `assets/textures` 读取内置纹理包并导入当前工程。
  内容浏览器材质缩略图由独立的 `MaterialThumbnailRenderer` 离屏渲染为 160×92
  球体纹理（宽高比与瓦片预览槽一致，按比例居中绘制以避免球体变形），函数
  缩略图从图结构按位置缩放绘制。`UeSyncStore` 把材质 → UE 资产
  的 exported/pushed/pulled 状态持久化到 `.materialforge/ue_sync.json`。
  节点/引脚缩略图由 `app/NodeThumbnail.h` 程序化绘制，用于节点库、节点头部、
  Inspector 和引脚工具提示。
- **自动保存**：dirty 图按配置间隔（默认 4 秒，可在偏好设置中关闭或调整
  2–120 秒）写入 `active.json` + `active.meta.json`；优先使用
  `%APPDATA%/MaterialForge/autosave`，不可写时回退到
  `<content root>/.materialforge/autosave`。正常保存/正常退出清理文件；
  异常退出后启动时读取 meta 并弹出恢复窗口，恢复后作为 dirty 文档打开。

- **CMD 控制台**：`Application::LogMessage` 记录带时间戳的 Info / Warning /
  Error；状态栏按钮显示错误/警告计数，展开后支持过滤、复制、清空、自动滚动
  和命令输入（`help`、`renderer`、`preview`、`library`、`console`、`stats`、
  `gpu`、`open`、`save`、`quit` 等）。
- **偏好设置**：`工程 → 偏好设置...`（`Ctrl+,`）选择渲染管线、主题、语言、
  节点库折叠、控制台显示和浮动预览；渲染管线选择会持久化并在启动时恢复。
- **浮动预览**：预览面板的“浮动”按钮创建可移动/缩放的 ImGui 窗口；拖回右侧
  预览区域或点击“停靠”恢复，位置/尺寸保存到设置。
- **节点库折叠**：左侧节点库可收起为窄栏，`Ctrl+L` 或 `视图 → 节点库` 切换，
  状态持久化。

## 9. UI 组织

### Application

- 窗口初始化、ImGui / 字体 / 样式；
- 文档会话：`EditorDocument` 保存材质或函数的图快照、路径与类型；活动文档
  与 `NodeGraphView` 共享实时 `MaterialGraph`，切换标签时深拷贝快照；关闭
  脏文档弹出保存 / 不保存 / 取消；
- 根布局：菜单栏、顶部工具栏、文档标签栏、三栏主体（左侧 Node Library、
  中间 Graph、右侧 Inspector）、底部全宽 Content Browser 瓦片栏和状态栏；
- 主题（Graphite / Midnight）与 UI 缩放：启动时按显示器 DPI / 工作区高度
  选择缩放，字体、样式、面板和卡片尺寸统一乘以 `Ui::Scale()`；
- 文件交互对话框（自绘 FileBrowser）；New Project 的文件夹选择在 Windows
  使用 `IFileOpenDialog(FOS_PICKFOLDERS)` 原生选择器；
- 工程会话：新建 / 打开 / 最近项目、文件树扫描、文件夹创建、外部文件
  导入、主材质切换与破损状态展示；
- 生成材质并驱动 PreviewRenderer；函数文档跳过 GLSL / 预览生成；
- 截图 CLI。

### NodeGraphView

- 管理 `ax::NodeEditor::EditorContext`；
- 把图节点/连线提交给 canvas；
- 节点视觉：分类色条标题、引脚类型色、连接状态、参数名/颜色预览；
- 创建连线前调用 `MaterialGraph::CanConnect`，不合法时 `RejectNewItem`；
- 删除时保护 MaterialOutput；
- 右键 / 双击打开可搜索的节点创建菜单；搜索按空白分词，支持多关键词、
  连字符压缩匹配和中文别名；
- 接收左侧节点库的 `MF_NODE_TYPE` ImGui drag-drop payload，在画布投放位置
  创建节点并显示投放边框；
- 接收内容浏览器 / 节点库的 `MF_TEXTURE_ASSET` 和 `MF_FUNCTION_ASSET`：
  前者创建 TextureObject + TextureSample，后者加载函数图并创建带动态引脚的
  MaterialFunctionCall；
- 未连接的 Math Float 输入在节点内显示内联数值框；编辑后调用
  `MarkContentChanged()`，值随图保存；
- 节点位置不吸附网格；图坐标按 UI 缩放显示，保存时归一化回基础坐标；
- 支持鼠标位置粘贴：复制选中节点及内部连线，`Ctrl+V` 在鼠标处重建；
- 支持 UE 风格快捷键（`3`/`4`/`T`/`V`/`S` 等 + 左键）和右键创建菜单；
- 编辑后同步图节点位置，更新 Inspector 选择。

### Inspector

- 按 `PropertySpec` 自动生成 Float / Vector / Color / Bool / String / AssetPath 控件；
- 参数名、常量、颜色等改动调用 `MarkContentChanged()`；
- **INPUT DEFAULTS** 表为每个未连接的数值输入生成对应类型的编辑器，写回
  `Pin::defaultValue`；
- 函数调用节点显示函数路径和 **Open & Reveal Material Function** 按钮；
- 引脚表显示实际类型、方向和连接来源；
- GLSL tab 显示生成源码、编译错误、warnings、纹理回退诊断和复制按钮；
- 函数文档隐藏 GLSL 预览，属性面板编辑 Function Input / Output 名称。

### 文件对话框

`FileBrowser` 是 ImGui 模态窗口实现，支持目录浏览、文件名输入、扩展名过滤、
双击打开、纯文件夹选择模式和相对 / 绝对路径。它用于：

- 打开 / 保存 / 导出材质图 JSON；
- 选择 TextureObject 的图片文件；
- 打开 `.mfproject`；
- 把文件 / 模型导入当前工程；
- 在非 Windows 平台为 New Project 选择父目录；
- 在文件夹选择模式下返回选定目录。

底部内容浏览器还负责工程材质的新建（`CreateProjectMaterial`）和最近工程列表。

### 内容浏览器

`CONTENT BROWSER` 是横跨窗口底部的 UE5 风格瓦片栏；上方主体仍保留 Node
Library / Graph / Inspector 三栏，分隔条可上下拖拽并通过设置持久化。
工程面板由 `Application` 绘制，数据行为集中在 `MaterialProject`：

- 通过 GLFW drop callback 接收 Windows Explorer 文件：在内容浏览器矩形内投放
  时调用 `MaterialProject::ImportFile` 复制到当前工程目录并登记；投放位置会
  在下一帧统一处理。纹理瓦片提供 ImGui drag payload `MF_TEXTURE_ASSET`，
  `NodeGraphView` 在画布投放时创建 `TextureObject` + `TextureSample` 并连线。
- `MaterialProject::Create` 负责创建目录、manifest 和默认主材质路径；
- 文件夹 / 材质名称按 UTF-8 保留中文、空格和常见符号，只拒绝 Windows
  文件系统非法字符（`\ / : * ? " < > |`）与保留名；错误以中文提示；
- `ListDirectory` 只读取工程 root 内的路径，目录优先排序；
- `ImportFile` outside-project 文件会复制、去重命名并登记；已有工程文件
  可以只登记而不复制；
- `Contains` / `RelativePathOf` 用规范化路径保证不会逃逸工程根目录；
- `RefreshIntegrity` 重新验证 `mainMaterial` 与所有 `required` 资源，并
  生成缺失引用列表；
- `RecentProjects` 把最近工程路径写到 `%APPDATA%\MaterialForge\recent_projects.json`
  （其他平台使用相应配置目录）。

瓦片不是传统表格，而是固定宽高的卡片（约 120×108）。文件夹、材质、模型、
工程和普通文件使用不同的 `ImDrawList` 图形；图片资源由 `stbi_load_from_memory`
解码后上传为 OpenGL 纹理，按路径缓存在 `Application::m_Thumbnails` 中，并在
GL context 销毁前统一释放。瓦片支持悬停/选中边框、文件名省略、类型标签、
工具提示、目录进入和材质/工程打开。

条目区上方是 `Up` 返回、可点击面包屑和搜索过滤器；搜索按文件名实时匹配。
未打开工程时，空状态显示最近工程瓦片和新建/打开工程操作。

空白区域的右键菜单通过手动检测（`IsWindowHovered(ChildWindows)` +
`!IsAnyItemHovered()`）触发，因此右键瓦片不会误弹出。菜单提供
**New Folder...** 和 **New Material...**；`New Material` 会调用
`CreateProjectMaterial`，写入新的起始材质 JSON、切换到该文档并更新
`mainMaterial`。

### 本地化与 UI 设置

`Localization` 提供全局 `Language`（English / Chinese）、`DetectSystemLanguage`
和 `Tr(key)`。中文翻译以英文原文为 key 保存，找不到时回退英文；菜单、工具栏、
节点库分组、节点描述、属性/引脚工具提示、检查器、文件浏览器、工程对话框、
内容浏览器、错误提示和状态栏都通过 `Tr` 绘制。节点类名、引脚名、UE class
等技术标识保留英文以便与 UE5 对照。`CjkGlyphRanges` 在 ImGui 常用 2500 字
范围上补充“编辑”“矢量”“菲涅尔”等界面实际使用的字形。

`AppSettingsStore` 把 `language` 与 `fileBarHeight` 写到
`%APPDATA%\MaterialForge\settings.json`，原子写入并在启动时恢复；语言切换
即时生效，不重建 OpenGL 上下文。

工程打开时，`Application` 优先按工程 root 解析材质里的贴图路径；找不到时
才回退到应用的 content root。这使工程可以携带自己的贴图而无需改变材质
JSON schema。

## 10. JSON 与 UE 桥接

`GraphSerializer` 负责 Schema 校验、属性类型校验、连线端点校验以及**原子加载**
（先在临时 `MaterialGraph` 构造，成功后 move 到目标；失败不破坏当前文档）。

关键设计：

- 节点 `type` 用 UE class 后缀；
- 节点 `ueClass` 完整类名，UE 脚本无需维护第二份映射；
- 每个引脚输出 `type`；Wildcard 额外输出 `resolvedType` 供调试/UI；
- 输入保存 `default`，输出不存；
- 连线同时保存 ID 和 pin 名称，便于诊断和容错；
- `material` 块保存 name / domain / blendMode / shadingModel / twoSided。

`.mfproject` 工程文件是独立于材质 JSON 的第二层格式，使用
`materialforge.project` v1，记录 `mainMaterial` 和相对路径资源清单。
它不是材质容器；打开工程会读取清单和文件树，但不修改材质 Schema。
详见 [PROJECT_FORMAT.md](PROJECT_FORMAT.md)。

UE Python 脚本的导入流程：

1. 校验 schema、版本、恰好一个 MaterialOutput；
2. 创建或清空目标 `unreal.Material`；
3. 按 `ueClass` 创建全部表达式并设置位置/属性；
4. TextureSample 通过指向 TextureObject 的连线找到源贴图，设置 `.texture`；
5. 遍历 links，内部连线用 `connect_material_expressions`，目标为 MaterialOutput
   的连线用 `connect_material_property`；
6. `recompile_material` + 保存。

详细 API 与已知限制见 [UE5_IMPORT.md](UE5_IMPORT.md)。

## 11. 测试策略

- `material_tests` 不依赖窗口，覆盖数据/类型/JSON/GLSL 文本；
- `shader_smoke_test` 创建隐藏 GLFW 窗口，因为 shader 编译本身必须由真实
  GPU 驱动验证；这能抓到文本测试碰不到的 GLSL 语法和链接错误；同时验证
  GLSL 4.60 头和 HLSL 计算着色器生成；
- shader 测试会遍历注册表，对每一个节点单独构建最小图并编译，防止新增节点
  只生成字符串但无法被 GLSL 接受；
- FBO 像素中心颜色断言验证了从 mesh、相机、TBO、shader 到 resolve 的整个预览
  路径，而不仅是 shader 编译；
- 工程测试在临时目录中创建工程、导入资源、往返加载、删除资源验证
  `BROKEN`/`Issues` 状态，并验证最近项目 JSON 的持久化、去重和清理。

## 12. 扩展与后续方向

可以按优先级扩展：

1. 撤销/重做（对 `MaterialGraph` 做命令模式或快照）；
2. Material Instance 参数导出与默认值管理；
3. 法线/粗糙度等非 sRGB 纹理采样配置；
4. `ComponentMask`、`AppendVector`、`Noise`、Custom HLSL 等节点；
5. UE 真实着色模型对齐（多光照、阴影、SSS/Unlit 模式）；
6. 工程内模型导入与预览、拖放导入、文件重命名 / 删除；
7. 自动化回归：记录文件 + ImGui 输入回放 + golden screenshot；
8. 把 core/codegen 编译为静态库供其他工具复用。
