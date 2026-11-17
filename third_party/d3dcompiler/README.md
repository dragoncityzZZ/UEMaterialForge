# D3DCompiler 47

`bin/d3dcompiler_47.dll` is the x64 DirectX Shader Compiler redistributable from
the Windows SDK (`Redist\D3D\x64`).

MaterialForge's DirectX 12 preview backend links against `d3dcompiler` and
compiles the generated HLSL at runtime. The DLL is copied next to the executable
so a machine does not need a separate Windows SDK or DirectX SDK installation.

The file is redistributed under the Microsoft Windows SDK / DirectX
redistributable terms. See the Windows SDK license accepted during installation
for the authoritative terms.
