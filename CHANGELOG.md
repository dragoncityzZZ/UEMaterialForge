# Changelog

## 2.6.7 — 2026-11-19

### 插件界面控制扩展

- 插件 API 新增：
  - `set_style_float`：圆角、内边距、间距、边框、滚动条等 ImGui 样式浮点值。
  - `set_node_style`：节点卡片圆角/边框和引脚圆角/边框。
  - `set_canvas_style`：节点画布背景、网格、节点背景、节点边框和选中色。
- 新增菜单栏、标题栏、滚动条和分隔线颜色覆盖键，主题可以覆盖更多程序区域。
- 示例插件 `Cute UI`：奶油粉彩、草莓色按钮、淡紫标题、粉彩节点分类、圆角卡片和糖果粉连接线，加载时自动应用到整个编辑器。

## 2.6.6 — 2026-11-19

### 插件界面控制

- 插件 API 新增视觉定制能力，同时保持旧插件兼容（追加字段）：
  - 主题切换：`graphite` / `midnight`。
  - ImGui 样式色覆盖：文本、窗口、边框、按钮、标签、强调色等。
  - 按节点类型或分类覆盖节点标题色。
  - 连接线颜色、粗细和直/曲造型。
  - UI 缩放。
- 生成的插件模板附带 `<plugin-id>.style` 示例命令，演示主题、节点颜色和连接线造型。
- 插件文档补充颜色格式（`0xAABBGGRR`）、样式键、API 空指针检查和示例代码。

### 工程

- 版本更新为 2.6.6。

## 2.6.5 — 2026-11-19

### 插件管理器

- 插件加载错误改为详细诊断：
  - Windows `LoadLibraryW` 错误码、系统消息和 DLL 架构（x86 / x64 / ARM64）。
  - 缺少依赖 DLL / VC++ 运行库、错误架构、缺少导出函数的明确提示。
  - `plugin.json` 缺失、JSON 语法错误、schema、schemaVersion、id、entry 字段错误。
  - `MaterialForgePluginInit` 返回 0 时显示插件通过 `api->log(host, 2, ...)` 写入的原因；
    没有日志时说明常见原因。
- Plugin Manager 中的错误改为换行显示，并增加 **复制错误** 按钮；加载成功但没有命令行/卸载导出时显示警告。
- 生成的插件模板和文档完全独立于主程序：
  - 自包含 ABI 头文件和完整函数签名。
  - CMake x64 + 静态 MSVC 运行库配置。
  - 完整构建、安装、manifest、生命周期和故障排查说明。
  - 初始化失败时模板会自动记录具体错误。
- `runtime_version` 使用主程序实际版本。

## 2.6.4 — 2026-11-18

### 安装包运行时

- 安装包自带 `vulkan-1.dll`（Khronos Vulkan loader），用户不需要安装 Vulkan SDK。
- 安装包自带 `d3dcompiler_47.dll`（Windows SDK DirectX 可再分发文件），
  DirectX 12 预览不需要单独安装 DirectX SDK。
- `tools/glslang/glslang.exe` 及其 VC++ 运行库继续随包分发。
- OpenGL 使用系统 GPU 驱动；DirectX 12 使用 Windows 10/11 的 D3D12/DXGI；
  需要对应显卡驱动，但不需要额外 SDK 或 VC++ Redistributable。

## 2.6.3 — 2026-11-18

### 新增

- `SingleLayerWaterMaterialOutput` 现在参与预览着色：Base Color 作为水下场景颜色，
  Scattering / Absorption / PhaseG 驱动水体透射和散射，并叠加程序化悬浮杂质；
  接到 Base Color 的纹理可以透过水面显示。
- TextureSample 新增直接 `texture` 属性；拖入纹理时直接设置该属性，不再生成
  额外的 TextureObject 节点。TextureSampleParameter2D 同样直接使用属性。
- Panner 增加 `Speed` 和 `Seed` 输入；Rotator 增加 `Time`、`Center`、`Speed`
  输入；Fresnel 增加 `Normal`、`Exponent`、`Base Reflect Fraction` 输入；
  Noise 增加 `Filter Width` 输入。

### 工程

- UE5 导入桥接支持 TextureSample 直接纹理属性和新增着色模型映射。
- 更新节点参考与 catalog。版本更新为 2.6.3。

## 2.6.2 — 2026-11-18

### 新增

- Material Settings 的 Shading Model 增加 `SingleLayerWater` 和 `ThinTranslucent`，
  与新增的单层水 / 薄半透明材质输出节点配套。
- UE5 导入桥接的 `SHADING_MODELS` 增加 `MSM_SINGLE_LAYER_WATER` 和
  `MSM_THIN_TRANSLUCENT` 映射。

## 2.6.1 — 2026-11-18

### 新增

- 新增特殊材质输出节点：
  - `SingleLayerWaterMaterialOutput`：Scattering Coefficients、Absorption Coefficients、PhaseG、Color Scale Behind Water。
  - `ThinTranslucentMaterialOutput`：Surface Coverage、Transmittance Color。
  - `ClearCoatNormalCustomOutput`：Clear Coat Normal。
- 节点库支持按“单层水 / 水材质 / 薄半透明 / 清漆”等中文关键词搜索。

### 修复

- 撤销粒度改为按实际编辑操作切分：鼠标拖动期间合并为一步，鼠标释放或键盘操作结束后立即落盘，
  不再把连续的添加、粘贴、删除等操作合并成过大的撤销步骤。
- 框选大量节点复制后，粘贴以整组内容的中心生成在鼠标位置，不再从鼠标处向右下偏移。

### 工程

- 节点定义总数更新为 94（90 个节点库可见），`node_catalog.json` 和节点参考文档同步更新。
- 版本更新为 2.6.1。

## 2.6.0 — 2026-11-17

### 修复

- 关闭最后一个材质标签后保持空工作区，不再自动生成新的起始材质蓝图。
- Undo / Redo 后保留节点编辑器视角：优先聚焦当前选中节点，而不是回到全局视图。
- TextureSample 的 Texture 输入现在显示当前贴图文件名；没有贴图时显示 `TextureObject`。
  检查器中的 Texture 输入也使用同样的显示规则。
- 节点信息、引脚信息和默认值工具提示改为锚定到对应的节点/引脚，不再随着鼠标乱飞。

### 新增

- 实时预览面板新增 **Live / 实时** 开关。
  - 开启：材质球每帧实时渲染，动画和参数变化立即显示。
  - 关闭：保留静态画面；旋转、缩放或重置材质球，或者修改材质/预览设置时才刷新。
  - 选项写入偏好设置，重启后保留。
- 新增 `CHANGELOG.md`、MIT `LICENSE` 和 GitHub 开源仓库说明。
- Inno Setup 安装器加入开始菜单卸载快捷方式、`UninstallDisplayName` 和可选的代码签名构建流程。

### 工程

- 版本号更新为 2.6.0。
- README 重新整理，增加程序介绍、截图、从 Releases 下载、卸载和 SmartScreen 说明。

## 2.5.0 — 初始开源版本

- UE5 风格材质节点编辑器，C++17 + Dear ImGui + imgui-node-editor。
- OpenGL / Vulkan / DirectX 12 实时预览。
- 内容浏览器、纹理包、纹理选择器、纹理通道查看器、多选文件操作。
- Material Instance、常量节点转参数、节点实时预览。
- UE5 JSON 导出 / Python 导入桥接。
- 本地 MCP 服务和插件管理器。
- Windows 安装程序与便携版。
