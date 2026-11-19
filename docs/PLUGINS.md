# Plugin Manager and Plugin SDK

MaterialForge plugins are folder-based Windows DLL plugins that can add CMD
console commands. Open **Edit → Plugin Manager...** to install, enable, reload,
uninstall, or create plugins.

## Installed plugin location

Plugins are installed under:

```text
%APPDATA%/MaterialForge/plugins/<plugin-id>/
```

If that directory cannot be created, the app falls back to
`<working-directory>/.materialforge/plugins/`.

## Plugin folder layout

```text
MyPlugin/
  plugin.json
  MyPlugin.dll
  README.md          (optional)
  docs/              (optional)
```

`plugin.json`:

```json
{
  "schema": "materialforge.plugin",
  "schemaVersion": 1,
  "id": "com.example.myplugin",
  "name": "My Plugin",
  "version": "1.0.0",
  "author": "Your Name",
  "description": "Short description",
  "entry": "MyPlugin.dll",
  "enabled": true
}
```

The `entry` path is relative to the plugin folder. A manifest without a DLL is
still listed, but reports an error instead of loading.

## C ABI

The public header is [src/plugin/MaterialForgePlugin.h](../src/plugin/MaterialForgePlugin.h).
A plugin must export:

```cpp
extern "C" __declspec(dllexport) int MaterialForgePluginInit(const MfPluginApi* api);
extern "C" __declspec(dllexport) void MaterialForgePluginShutdown();
```

`MaterialForgePluginInit` receives the host API. Return `1` on success and `0`
on failure. During initialization the plugin can register console commands:

```cpp
api->register_command(api->host, "myplugin.hello",
                      "Print a greeting.", &OnHello, nullptr);
```

`OnHello` gets the text after the command name as `args` and the `user` pointer
passed at registration. Use `api->log(api->host, level, message)` to write to
the MaterialForge CMD console (`level` 0 = info, 1 = warning, 2 = error).

`api->runtime_version(api->host)` returns the host version string.

### Visual customization (API v1 appended fields)

Newer hosts append optional UI functions to `MfPluginApi`. Check each pointer is
non-null before use:

```cpp
if (api->set_theme) {
    api->set_theme(api->host, "midnight"); // or "graphite"
}
if (api->set_style_color) {
    api->set_style_color(api->host, "accent", 0xFFFFB060u); // 0xAABBGGRR
}
if (api->set_node_color) {
    api->set_node_color(api->host, "Math", 0xFFD080FFu);       // node type
    api->set_node_color(api->host, "Textures", 0xFF60B0FFu);   // or category
}
if (api->set_link_style) {
    api->set_link_style(api->host, 0xFFFFB060u, 2.5f, 1);      // 0 curved, 1 straight
}
```

Supported style keys are `text`, `text_disabled`, `window_bg`, `child_bg`,
`popup_bg`, `border`, `frame_bg`, `button`, `button_hovered`, `button_active`,
`header`, `header_hovered`, `tab`, `tab_selected`, and `accent`. Node colors can
use a node type or a category; type overrides win. `set_ui_scale(host, scale)`
changes the ImGui scale (clamped).

The generated plugin includes a `<plugin-id>.style` command demonstrating these
calls.

## Create a plugin project

1. Open **Edit → Plugin Manager...**.
2. Click **Create Plugin...**.
3. Choose a name, unique ID, optional author/description, and a parent folder.
   The **Browse...** button uses the native Windows folder picker.
4. Click **Create**.

The wizard creates:

```text
<parent>/<PluginName>/
  plugin.json
  CMakeLists.txt
  README.md
  docs/PLUGIN_API.md
  include/MaterialForgePlugin.h
  src/plugin.cpp
```

The template registers a `<plugin-id>.hello` console command.

Build on Windows:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

The CMake template copies the built DLL next to `plugin.json`. Use **Add
Plugin...** to install the folder, then **Reload All** if it was already
installed.

## Install / uninstall

- **Add Plugin...** selects a plugin folder and copies it to the installed
  plugin directory; the manifest ID determines the destination folder name.
- **Enabled** writes `enabled` back to `plugin.json` and reloads plugins.
- **Reload** reloads all plugins.
- **Uninstall** unloads the DLL and removes the installed folder. There is no
  undo; keep your source project elsewhere.
