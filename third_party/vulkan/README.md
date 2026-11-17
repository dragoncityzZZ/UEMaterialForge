# Vulkan headers

`include/vulkan` and `include/vk_video` are vendored from
[KhronosGroup/Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers).

MaterialForge loads `vulkan-1.dll` dynamically, so no Vulkan import library is
required at build time. The Vulkan preview backend compiles its generated HLSL
compute shader to SPIR-V with the bundled `glslang.exe`.

License: Apache-2.0 OR MIT; see `LICENSE.md`.
