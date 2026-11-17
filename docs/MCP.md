# MCP Server

MaterialForge ships a small MCP (Model Context Protocol) server that lets other
local tools inspect the open project, create materials, edit node graphs and
import Unreal Engine clipboard text.

The server is **off by default**.

## Enable

1. Open **Preferences** (Ctrl+,) and select the **MCP** tab (or search for
   `mcp` in the settings search box).
2. Tick **Enable MCP server**.
3. The default endpoint is `http://127.0.0.1:3011/mcp`. Change the port with
   the **Port** field and press **Apply Port** if needed.

The server binds to `127.0.0.1` only and uses JSON-RPC 2.0 over HTTP
(Streamable HTTP style with a JSON response). Notifications are answered with
HTTP 202; tool calls with HTTP 200.

## Client configuration

Most local MCP clients can use a URL-style server entry:

```json
{
  "mcpServers": {
    "materialforge": {
      "url": "http://127.0.0.1:3011/mcp"
    }
  }
}
```

## Tools

| Tool | Arguments | Description |
|---|---|---|
| `mf_status` | none | Version, open project, active document, graph stats, preview backend. |
| `mf_list_materials` | `folder?` | List `.json` material/function assets in the project. |
| `mf_create_material` | `name`, `folder?`, `template?`, `graph?` | Create a material file. `template` is `default` or `empty`; `graph` accepts a `materialforge.ue5.material` document. |
| `mf_get_graph` | `path` | Read a material graph JSON document. |
| `mf_set_graph` | `path`, `graph` | Replace a material graph. |
| `mf_add_node` | `path`, `type`, `x`, `y`, `properties?` | Add a node by registry type. |
| `mf_connect` | `path`, `fromNode`, `fromPin`, `toNode`, `toPin` | Connect an output pin to an input pin. |
| `mf_import_ue5_text` | `path`, `text` | Parse UE material-node clipboard text into a graph. |

`path` is project-relative (for example `materials/M_Water.json`), or an
absolute path inside the open project.

### Example: initialize

```bash
curl -X POST http://127.0.0.1:3011/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"curl","version":"1.0"}}}'
```

### Example: create a material

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "tools/call",
  "params": {
    "name": "mf_create_material",
    "arguments": {"name": "M_AutoCreated", "folder": "materials", "template": "default"}
  }
}
```

## Safety / scope

- Local loopback only; no remote bind option.
- The server only reads/writes inside the currently open project.
- Tools that would overwrite an active document with unsaved changes return an
  error; save or close the document first.
- Tool calls are marshalled onto the editor's main thread, so graph and project
  state stay consistent with the UI.
