# ACP Channel (Agent Communication Protocol)

MCP-compatible Agent Communication Protocol channel. Enables inter-agent communication using the standard JSON-RPC-based protocol over HTTP and stdio transports.

## Protocol

ACP is compatible with the Model Context Protocol (MCP), supporting the same JSON-RPC message format.

### Supported Methods

| Method | Description |
|--------|-------------|
| `initialize` | Handshake with client info and capabilities |
| `tools/list` | List available tools on this agent |
| `tools/call` | Invoke a tool by name with JSON arguments |
| `resources/list` | List available resources (file access) |
| `resources/read` | Read a resource by URI |

### Transports

| Transport | URL/Endpoint | Description |
|-----------|-------------|-------------|
| **HTTP** | `http://host:port/mcp` | JSON-RPC over HTTP POST |
| **Stdio** | stdin/stdout | For local process-based MCP clients |

## Configuration

```jsonc
{
  "channels": {
    "acp": {
      "enabled": true,
      "http_port": 8190,
      "http_host": "0.0.0.0"
    }
  }
}
```

## Usage

### HTTP Transport

```bash
# Initialize
curl -X POST http://localhost:8190/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","clientInfo":{"name":"my-client","version":"1.0"}}}'

# List tools
curl -X POST http://localhost:8190/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'

# Call a tool
curl -X POST http://localhost:8190/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"read_file","arguments":{"path":"/tmp/test.txt"}}}'
```

### Stdio Transport

Configure your MCP client to spawn `./build/primagen acp-server` as a subprocess. The agent will communicate via stdin/stdout using JSON-RPC messages.

## Capabilities

- Exposes all built-in tools (file ops, exec, etc.) as MCP `tools/list` and `tools/call`
- Exposes workspace files as MCP `resources/list` and `resources/read`
- Returns tool results in MCP-compliant format

## Troubleshooting

| Problem | Solution |
|---------|----------|
| Connection refused | Verify HTTP port matches config; check firewall |
| Method not found | Check the `method` field is a supported method name |
| Tool call fails | Verify tool name matches exactly; check args JSON schema |
| Stdio transport not working | Ensure client spawns the correct binary path `./build/primagen acp-server` |