# Vulkan loader

`bin/vulkan-1.dll` is the Khronos Vulkan loader (version 1.4.309.0) used by the
Vulkan preview backend.

It is copied next to `MaterialForge.exe` so Windows can load the Vulkan runtime
without a separate Vulkan SDK installation. A Vulkan-capable GPU driver / ICD is
still required for the Vulkan backend to create a device; the loader itself is
the platform runtime.

License: Apache-2.0 OR MIT; see the Vulkan-Headers license in the parent
directory (`third_party/vulkan/LICENSE.md`).
