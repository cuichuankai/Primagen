# MCP Tools

MCP (Model Context Protocol) client plugin. Connects to external MCP servers to discover and use their tools.

## Tool Definition

- **Name**: `mcp`
- **Description**: Call an MCP server tool by name with JSON arguments

## Configuration

```jsonc
{
  "plugins": {
    "mcp_tools": {
      "enabled": false,
      "config": {
        "servers": [
          {
            "id": "my-mcp-server",
            "transport": "stdio",
            "command": "npx",
            "args": ["-y", "@modelcontextprotocol/server-filesystem", "/tmp"],
            "env": {}
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
  }
}
```

| Field | Description |
|-------|-------------|
| `id` | Unique identifier for the MCP server instance |
| `transport` | Transport protocol for connecting to the server instance |
| `command` | Command to spawn the MCP server |
| `args` | Arguments for the command |
| `env` | Environment variables for the server process |
| `url` | URL for the server instance |
| `request_url` | URL for the server instance to send requests |
| `headers` | HTTP headers for the server instance to send requests with |

## Usage

The agent discovers tools from connected MCP servers and calls them:

```json
{
  "server": "my-mcp-server",
  "tool": "read_file",
  "arguments": {
    "path": "/tmp/test.txt"
  }
}
```

Each MCP server's tools are registered under its server name to avoid conflicts.

## Notes

- `mcp_tools` registers MCP tools with `mcp_` name prefix.
- The plugin also registers `mcp_list_resources`, `mcp_read_resource`, and `mcp_list_prompts`.
- Reserved headers such as `Host` and `Content-Length` cannot be overridden.

## Troubleshooting

| Problem | Solution |
|---------|----------|
| Server fails to start | Verify `command` and `args` are correct; test manually first |
| MCP tools not discovered | Check `agent.log` for server connection errors |
| Tool call returns error | Verify tool name and arguments match the server's schema |