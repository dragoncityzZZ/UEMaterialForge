# Third-party dependencies

All dependencies are vendored so the project configures and builds without
network access. The versions below are the versions currently checked in.

| Directory | Version | Purpose | License |
|---|---|---|---|
| `imgui/` | Dear ImGui **v1.91.9b** | UI toolkit, GLFW/OpenGL backends, `imgui_stdlib` | MIT (`imgui/LICENSE.txt`) |
| `imgui-node-editor/` | imgui-node-editor **v0.9.4** | Node canvas / link editing | MIT (`imgui-node-editor/LICENSE`) |
| `glfw/` | GLFW **3.4** | Window + OpenGL context + input | zlib/libpng (`glfw/LICENSE.md`) |
| `glad/` | glad 2 generated loader for **OpenGL 4.6 core** | OpenGL function loading | Generated loader by glad (MIT); Khronos headers retain their notices |
| `vulkan/` | Vulkan-Headers (main) | Vulkan types / constants for the Vulkan preview backend | Apache-2.0 OR MIT (`vulkan/LICENSE.md`) |
| `glslang/` | glslang **16.6.0** Windows x64 | HLSL → SPIR-V shader compilation at runtime | Apache-2.0/BSD/MIT (`glslang/LICENSE`) |
| `json/` | nlohmann/json **v3.12.0** | Graph JSON serialization | MIT (`json/LICENSE.MIT`) |
| `stb/` | current `stb_image.h` / `stb_image_write.h` | Image loading and screenshot encoding | public domain / MIT (see file headers) |

## Refreshing dependencies

- Dear ImGui: download the release archive, keep the root `.cpp/.h` files and
  `backends/imgui_impl_glfw.*`, `backends/imgui_impl_opengl3*`, plus
  `misc/imgui_stdlib.*`.
- imgui-node-editor: keep `imgui_canvas.*`, `imgui_node_editor*`,
  `imgui_extra_math.*` and `crude_json.*` (the CMake target compiles
  `crude_json.cpp` because node settings serialization depends on it).
- glad: regenerate with glad 2 using
  `python -m glad --api gl:core=4.6 --out-path third_party/glad c`.
- Vulkan-Headers: copy `include/vulkan` and `include/vk_video` from
  https://github.com/KhronosGroup/Vulkan-Headers.
- glslang: replace `third_party/glslang/bin/glslang.exe` with the standalone
  Windows x64 release binary; CMake copies it to `tools/glslang/` next to the
  executable.
- The CMake targets are defined in the top-level `CMakeLists.txt`.

## Licensing

The vendored third-party source retains its original license. Any distribution
of MaterialForge should keep those license files alongside the application.
