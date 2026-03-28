# mcp_tools

`mcp_tools` is a tool plugin that connects Primagen to MCP servers and registers remote MCP capabilities into the tool registry at runtime.

## Path

- Source: `plugins/tools/mcp_tools/`
- Build output: `build/.primagen/plugins/tools/mcp_tools.so`
- Runtime install: `.primagen/plugins/tools/mcp_tools.so`

## Build

```bash
cd plugins/tools/mcp_tools
make
```

Build all tool plugins from repository root:

```bash
make -C plugins tools
```

## Configuration

Configure this plugin in `.primagen/config.json`:

```json
{
  "plugins": [
    {
      "plugin_id": "mcp_tools",
      "enabled": true,
      "config": {
        "servers": [
          {
            "id": "filesystem",
            "transport": "stdio",
            "command": "npx",
            "args": ["-y", "@modelcontextprotocol/server-filesystem", "/Users/you/workspace"]
          },
          {
            "id": "remote_tools",
            "transport": "websocket",
            "url": "ws://127.0.0.1:9000/mcp"
          },
          {
            "id": "remote_sse_tools",
            "transport": "sse",
            "url": "http://127.0.0.1:8000/sse",
            "request_url": "http://127.0.0.1:8000/messages",
            "headers": {
              "Authorization": "Bearer <token>"
            }
          },
          {
            "id": "remote_streamable_http_tools",
            "transport": "streamable_http",
            "url": "https://example.com/mcp",
            "request_url": "https://example.com/mcp",
            "headers": {
              "Authorization": "Bearer <token>"
            }
          }
        ]
      }
    }
  ]
}
```

## Server Fields

- `id`: unique MCP server identifier.
- `transport`: `stdio`, `websocket`, `sse`, or `streamable_http`.
- `command`: required for `stdio`.
- `args`: optional for `stdio`; for `sse` and `streamable_http`, used as extra request args.
- `url`: required for `websocket`, `sse`, `streamable_http`.
- `request_url`: optional for `sse` and `streamable_http`; defaults to `url`.
- `headers`: optional object for custom HTTP headers on `sse` and `streamable_http`.

## Notes

- `mcp_tools` registers MCP tools with `mcp_` name prefix.
- The plugin also registers `mcp_list_resources`, `mcp_read_resource`, and `mcp_list_prompts`.
- Reserved headers such as `Host` and `Content-Length` cannot be overridden.
