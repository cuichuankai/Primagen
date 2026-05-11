# Echo Tool

A debug/test tool that echoes back the input parameters. Useful for verifying the tool invocation pipeline and as a minimal example for plugin development.

## Tool Definition

- **Name**: `echo`
- **Parameters**: `message` (string, required)
- **Returns**: The input `message` as-is

## Configuration

```jsonc
{
  "plugins": {
    "echo_tool": {
      "enabled": false
    }
  }
}
```

## Usage

The agent can invoke this tool:
```json
{"message": "Hello, world!"}
```

Returns:
```json
{"echo": "Hello, world!"}
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| Tool not available | Verify `enabled: true` in config and `.so` is loaded; check WebUI → Plugins → Tools |
| `message` parameter missing | The `message` field is required; check JSON schema |