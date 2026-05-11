# Hello Command

A minimal debug/test command plugin example. Prints "hello" to the console.

## Configuration

```jsonc
{
  "plugins": {
    "hello_command": {
      "enabled": false
    }
  }
}
```

## Usage

When enabled, this command is registered in the CLI:

```bash
./build/primagen hello
```

Output:
```
Hello from hello_command plugin!
```

This plugin serves as a minimal reference implementation for developers creating their own command plugins. See [plugins/README.md](../../README.md) for the full development guide.

## Troubleshooting

| Problem | Solution |
|---------|----------|
| Command not found | Verify `enabled: true` in config and `.so` is loaded |