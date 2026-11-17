# glslang (Vulkan shader compiler)

`bin/glslang.exe` is the standalone Windows x64 release binary from
[KhronosGroup/glslang](https://github.com/KhronosGroup/glslang), version 16.6.0
(`glslang-16.6.0-windows-x86_64-release.zip`).

MaterialForge uses it at runtime to compile the generated HLSL preview compute
shader to SPIR-V for the Vulkan pipeline. The binary is invoked only when the
Vulkan preview backend compiles a material shader; it is not required for the
OpenGL or DirectX 12 backends.

The matching Microsoft Visual C++ runtime DLLs (`msvcp140.dll`,
`vcruntime140.dll`, `vcruntime140_1.dll`) are bundled in `bin/` because the
standalone glslang binary is built against the dynamic CRT. They are copied to
`tools/glslang/` next to the executable.

Licensing: see `LICENSE` (Khronos glslang license, Apache-2.0/BSD/MIT components).
The Microsoft runtime DLLs follow the Microsoft Visual Studio redistribution
terms.
