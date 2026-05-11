# Stdout Channel

Output-only channel that writes agent replies to stdout. Useful for scripting, piping, and headless automation.

## Configuration

```jsonc
{
  "channels": {
    "stdout": {
      "enabled": false
    }
  }
}
```

## Usage

When enabled, all agent reply messages are piped to stdout in JSON format. Each line is a JSON object:

```json
{"channel":"stdout","chat_id":"default","message":"Hello, how can I help?"}
```

### Scripting Example

```bash
# Pipe agent output to a log file
./build/primagen agent 2>/dev/null | grep '"message"' > agent_log.jsonl

# Process with jq
./build/primagen agent | jq '.message'
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| No output | Verify `enabled: true` in config; check other channels aren't consuming replies |
| Output mixed with logs | Agent logs go to stderr; stdout channel output goes to stdout — use `2>/dev/null` for clean output |