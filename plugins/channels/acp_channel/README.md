# ACP Channel Plugin

Provides ACP HTTP endpoints as a dynamically loaded channel plugin.

## Config

```json
{
  "plugin_id": "acp_channel",
  "enabled": true,
  "config": {
    "port": 8080,
    "host": "127.0.0.1"
  }
}
```

Use `config.port` to set ACP listen port.
