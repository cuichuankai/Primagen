# Primagen

<p align="center">
  <img src="Primagen.png" alt="Primagen logo" width="300">
</p>  

My original intention was Primitive Genesis(元婴), BUT ...  
Primagen(Turok 2: Seeds of Evil), the ancient cosmic creature imprisoned in the underground city, harbors an ambition to annihilate the universe, driven by an extreme lust for power.  
When technology—such as nuclear energy or genetic engineering—breaks free from moral constraints, it can turn into an instrument of destruction.
Similarly, unregulated AI Agents may inflict catastrophic harm.  
If we can restrain Primagen's malice and guide him toward goodness, a completely different future will unfold.  
Primagen lays bare the inherent evil of human nature.  
Yet even the flower of evil can bear the fruit of good, if carefully nurtured and cultivated.

> Primagen（恐龙猎人2:邪恶之源），这头被囚禁于地底之城的远古宇宙生灵，其毁灭宇宙的狂想，本质是对权力极致贪婪的终极投射。\
> 科技一旦挣脱道德的缰绳 —— 无论是核能、基因工程，还是如今的 AI Agent—— 都将从文明的利刃，蜕变为自我毁灭的凶器。\
> 可真正的启示，不在于镇压这头远古巨兽，而在于驯化与引导：压制其恶，唤醒其善，世界便会走向截然不同的未来。\
> Primagen 映照的，从来不是外星怪物，而是人性深处的幽暗本源。\
> 恶本是天生的种子，但若以理性与良知浇灌，恶之花，亦可结出善之果。

Primagen is a pure C implementation of an extensible AI agent runtime.  
It combines a message-driven agent loop, dynamic plugin loading, MCP tool plugin integration, and optional ACP HTTP APIs.

## What Primagen Provides

- Multi-turn agent loop with tool calling
- Thread-safe message bus between channels and core runtime
- Dynamic plugin system for channels, tools, and commands
- Session persistence (`.primagen/sessions`)
- Two-layer memory storage (`.primagen/memory`)
- MCP client integration for external tool servers (via `mcp_tools` plugin)
- Optional ACP server with OpenAI-compatible endpoints

## Project Structure

```text
Primagen/
├── src/
│   ├── agent/          # Agent loop and built-in registration
│   ├── bus/            # Message bus
│   ├── channels/       # Built-in channels (currently console)
│   ├── cli/            # CLI command handlers
│   ├── common/         # Logger, string, utilities
│   ├── config/         # Config loading and defaults
│   ├── context/        # Prompt/context assembly
│   ├── cron/           # Scheduled task service
│   ├── memory/         # MEMORY.md + HISTORY.md
│   ├── plugin/         # Plugin manager and loaders
│   ├── providers/      # LLM provider implementation
│   ├── session/        # Session persistence
│   ├── skills/         # Skill loading
│   ├── subagent/       # Subagent manager
│   ├── tools/          # Built-in tool implementations
│   └── main.c          # Entry point
├── plugins/
│   ├── channels/       # Channel plugins (Feishu, DingTalk, Telegram, ...)
│   ├── tools/          # Tool plugins (web_tools, mcp_tools, ...)
│   └── commands/       # Command plugins
├── tests/              # Unit tests
└── Makefile
```

## Architecture at a Glance

```text
Inbound Channel
    -> MessageBus (inbound queue)
    -> AgentLoop (context + LLM + tools)
    -> MessageBus (outbound queue)
    -> Outbound Channel
```

On startup, Primagen initializes config, session/memory, plugins, cron service, and skills, then starts agent and outbound threads.

## Built-in vs Plugin Components

### Built-in channel
- `console` (compiled in)

### External channel plugins
- `feishu` (`plugins/channels/feishu_channel`)
- `dingtalk` (`plugins/channels/dingtalk_channel`)
- `telegram`, `discord`, `slack`, `email`, `stdout`

All external plugins are loaded recursively from:

```text
.primagen/plugins/
```

## Plugin System Overview

Primagen uses dynamic plugins (`.so`) to extend runtime capability without recompiling the main binary.

- **Plugin types**: `channels`, `tools`, `commands`
- **Load path**: `.primagen/plugins/<type>/`
- **Enable switch**: `plugins[].enabled` in `.primagen/config.json`
- **Per-plugin config**: `plugins[].config` object

Minimal plugin configuration shape:

```json
{
  "plugins": [
    {
      "plugin_id": "example_plugin",
      "enabled": true,
      "config": {}
    }
  ]
}
```

## Key Channel Plugins

### Feishu (`feishu_channel`)

- Path: `plugins/channels/feishu_channel`
- Purpose: Feishu/Lark bot inbound + outbound messaging
- Key config: `config.app_id`, `config.app_secret`, optional `config.use_card`
- Outbound channel name: `feishu`

### DingTalk (`dingtalk_channel`)

- Path: `plugins/channels/dingtalk_channel`
- Purpose: DingTalk inbound + outbound messaging
- Key config: `config.clientId`, `config.clientSecret`
- Outbound channel name: `dingtalk`

### WebUI (`webui_channel`)

- Path: `plugins/channels/webui_channel`
- Purpose: Local web control panel and chat UI
- Key config: `config.port` (default `16714`)
- Access URL: `http://127.0.0.1:<port>/`

### ACP (`acp_channel`)

- Path: `plugins/channels/acp_channel`
- Purpose: OpenAI-compatible HTTP API endpoints for tools/chat
- Key config: `config.port` (default `8080`), `config.host` (default `127.0.0.1`)
- Typical endpoints: `/v1/health`, `/v1/tools/list`, `/v1/tools/call`, `/v1/chat/completions`

## Build

```bash
make
make test
make clean
make package
make android
```

## Build and Install Plugins

```bash
cd plugins
make
make install
```

The plugin build output is copied to:

```text
build/.primagen/plugins/<type>/
```

Runtime installation copies `.so` files to:

```text
.primagen/plugins/<type>/
```

## Run

```bash
./build/primagen onboard
./build/primagen agent
```

Other commands:

```bash
./build/primagen gateway
./build/primagen status
./build/primagen channels status
```

## Runtime Paths

- Config: `.primagen/config.json`
- Memory: `.primagen/memory/`
- Sessions: `.primagen/sessions/`
- Logs: `.primagen/log/`
- Plugins: `.primagen/plugins/`

## MCP Tools Plugin Configuration Example

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
            "args": ["-y", "@modelcontextprotocol/server-filesystem", "/Users/cuick/workdir/AI/Primagen"]
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
              "Authorization": "Bearer <token>",
              "X-Api-Key": "<api-key>"
            }
          },
          {
            "id": "remote_streamable_http_tools",
            "transport": "streamable_http",
            "url": "https://example.com/mcp",
            "request_url": "https://example.com/mcp",
            "headers": {
              "Authorization": "Bearer <token>",
              "X-Api-Key": "<api-key>"
            }
          }
        ]
      }
    }
  ]
}
```

- `mcp_tools` reads MCP server definitions from `plugins[].config.servers`.
- For `stdio`, Primagen starts the MCP server process with `command + args`.
- For `websocket`, Primagen connects directly to `url` and exchanges MCP JSON-RPC messages over WebSocket text frames.
- For `sse`, Primagen reads server events from `url` and sends JSON-RPC requests to `request_url` (defaults to `url` when omitted).
- For `streamable_http`, Primagen sends JSON-RPC requests over HTTP POST and supports both JSON and `text/event-stream` responses; `request_url` defaults to `url` when omitted.
>For `sse` and `streamable_http`, you can add a `headers` object to send custom HTTP headers (for example `Authorization`).  
> Custom headers are sent as real HTTP headers (not in JSON payload); `Host` and `Content-Length` are reserved and cannot be overridden.  

## Plugin Configuration Example

```json
{
  "plugins": [
    {
      "plugin_id": "feishu_channel",
      "enabled": true,
      "config": {
        "app_id": "cli_xxx",
        "app_secret": "xxx",
        "use_card": false
      }
    },
    {
      "plugin_id": "dingtalk_channel",
      "enabled": true,
      "config": {
        "clientId": "dingxxxx",
        "clientSecret": "xxxx"
      }
    },
    {
      "plugin_id": "webui_channel",
      "enabled": true,
      "config": {
        "port": 16714
      }
    },
    {
      "plugin_id": "acp_channel",
      "enabled": true,
      "config": {
        "host": "127.0.0.1",
        "port": 8080
      }
    },
    {
      "plugin_id": "mcp_tools",
      "enabled": true,
      "config": {
        "servers": [
          {
            "id": "remote_streamable_http_tools",
            "transport": "streamable_http",
            "url": "https://example.com/mcp"
          }
        ]
      }
    }
  ]
}
```

## ACP HTTP API

Start with ACP:

```bash
./build/primagen agent
```

Endpoints:

- `GET /v1/health`
- `GET /v1/tools/list`
- `POST /v1/tools/call`
- `POST /v1/chat/completions`

## Documentation

- `plugins/README.md` for plugin development and build workflow
- `plugins/channels/feishu_channel/README.md` for Feishu plugin setup
- `plugins/channels/dingtalk_channel/README.md` for DingTalk plugin setup
- `plugins/channels/webui_channel/README.md` for WebUI plugin setup
- `plugins/channels/acp_channel/README.md` for ACP plugin setup
- `plugins/tools/mcp_tools/README.md` for MCP tool plugin setup
