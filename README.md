# Primagen

<p align="center">
  <img src="Primagen.png" alt="Primagen logo" width="300">
</p>

My original intention was Primitive Genesis(元婴), BUT ...\
Primagen, the ancient cosmic creature imprisoned in the underground city, harbors an ambition to annihilate the universe, driven by an extreme lust for power.\
When technology—such as nuclear energy or genetic engineering—breaks free from moral constraints, it can turn into an instrument of destruction.
Similarly, unregulated AI Agents may inflict catastrophic harm.\
If we can restrain Primagen's malice and guide him toward goodness, a completely different future will unfold.\
Primagen lays bare the inherent evil of human nature.\
Yet even the flower of evil can bear the fruit of good, if carefully nurtured and cultivated.

> Primagen，这头被囚禁于地底之城的远古宇宙生灵，其毁灭宇宙的狂想，本质是对权力极致贪婪的终极投射。\
> 科技一旦挣脱道德的缰绳 —— 无论是核能、基因工程，还是如今的 AI Agent—— 都将从文明的利刃，蜕变为自我毁灭的凶器。\
> 可真正的启示，不在于镇压这头远古巨兽，而在于驯化与引导：压制其恶，唤醒其善，世界便会走向截然不同的未来。\
> Primagen 映照的，从来不是外星怪物，而是人性深处的幽暗本源。\
> 恶本是天生的种子，但若以理性与良知浇灌，恶之花，亦可结出善之果。

This is a pure C implementation of the original nanobot project.

## Project Structure

```
Primagen/
├── src/                          # Source code
│   ├── agent/                    # Agent loop (ReAct paradigm)
│   ├── bus/                      # Message bus (thread-safe async queues)
│   ├── channels/                 # Communication channels (builtin)
│   │   ├── console.c             # Console/CLI channel
│   │   ├── feishu.c              # Feishu (Lark) channel
│   │   └── feishu_ws.c           # Feishu WebSocket channel
│   ├── cli/                      # CLI commands
│   ├── common/                   # Common utilities (string, logger, etc.)
│   ├── config/                   # Configuration loader
│   ├── context/                  # Context builder for prompts
│   ├── cron/                     # Cron service for scheduled tasks
│   ├── heartbeat/                # Heartbeat service
│   ├── memory/                   # Two-layer memory system
│   ├── mcp/                      # MCP (Model Context Protocol) client
│   ├── plugin/                   # Plugin manager (dynamic loading)
│   ├── providers/                # LLM providers (OpenAI-compatible)
│   ├── session/                  # Session persistence (JSONL)
│   ├── skills/                   # Skills loader
│   ├── subagent/                 # Subagent manager
│   ├── tools/                    # Tool registry and implementations
│   ├── vendor/                   # Third-party libraries
│   │   ├── cJSON/                # JSON parsing
│   │   └── mongoose/             # HTTP client
│   ├── include/                  # Common headers
│   └── main.c                    # Entry point
├── plugins/                      # External plugins (dynamically loaded)
│   ├── channels/                 # Channel plugins
│   │   ├── telegram_channel/     # Telegram channel plugin
│   │   ├── email_channel/        # Email channel plugin
│   │   ├── discord_channel/      # Discord channel plugin
│   │   ├── slack_channel/        # Slack channel plugin
│   │   └── dingtalk_channel/     # DingTalk channel plugin
│   ├── tools/                    # Tool plugins
│   │   └── echo_tool/            # Demo tool plugin
│   └── commands/                 # Command plugins
│       └── hello_command/        # Demo command plugin
├── Makefile                      # Build script
├── README.md                     # This file
└── PLUGIN_REFACTORING_SUMMARY.md # Plugin system documentation
```

## Architecture Overview

### Core Modules

| Module | Directory | Purpose |
|--------|-----------|---------|
| Agent Loop | `src/agent/` | ReAct reasoning-acting cycle |
| Message Bus | `src/bus/` | Thread-safe async message queues (inbound/outbound) |
| Context Builder | `src/context/` | Prompt assembly from identity, memory, skills, history |
| Tool Registry | `src/tools/` | Dynamic tool registration and execution |
| Session Manager | `src/session/` | JSONL-based session persistence |
| Memory System | `src/memory/` | Two-layer memory (MEMORY.md for facts, HISTORY.md for logs) |
| Subagent Manager | `src/subagent/` | Background task delegation |
| Cron Service | `src/cron/` | Scheduled task management |
| Skills Loader | `src/skills/` | Extensible skill system (SKILL.md format) |
| MCP Client | `src/mcp/` | Model Context Protocol for external tools |
| Plugin Manager | `src/plugin/` | Dynamic plugin loading (.so files) |
| LLM Provider | `src/providers/` | OpenAI-compatible API interface |

### Plugin System

Primagen has a plugin-based architecture that allows extending tools, channels, and commands at runtime.

| Plugin Type | Location | Loading |
|-------------|----------|---------|
| Builtin Channels | `src/channels/` | Compiled into binary |
| External Channels | `plugins/channels/` | Dynamically loaded .so |
| External Tools | `plugins/tools/` | Dynamically loaded .so |
| External Commands | `plugins/commands/` | Dynamically loaded .so |

### Data Flow

```
User Input → InboundMessage → MessageBus → AgentLoop
                                    ↓
Context: Identity + Bootstrap Files + Memory + Skills + History
                                    ↓
                              LLM Call (OpenAI API)
                                    ↓
Tool Execution: file ops, shell, web, subagent, cron, skill, memory, MCP, plugins
                                    ↓
OutboundMessage → MessageBus → Channel (CLI/Telegram/etc.) → User
                                    ↓
Session Persistence (JSONL) + Memory Consolidation
```

## Features Implemented

- Agent Loop with multi-turn ReAct Paradigm
- MessageBus for decoupling channels and agent core
- ContextBuilder for assembling prompts from Identity, Memory, Skills, and History
- Tool registration mechanism (Filesystem, Shell, Web, Subagent, Cron, Skill, Memory)
- MCP (Model Context Protocol) Client integration for external tools
- Session persistence in JSONL format (filtered for clarity)
- Two-layer Memory System (Long-term facts & Short-term history)
- Subagent Manager for background tasks
- Cron Service for scheduled tasks
- Real LLM Provider (OpenAI/Brave integration)
- Flexible Configuration System with environment variable support
- Comprehensive Logging
- **Plugin System** for extensible tools, channels, and commands

## Builtin vs External Components

### Builtin Channels (compiled into binary)
| Channel | File | Status |
|---------|------|--------|
| console | `src/channels/console.c` | ✅ Active |
| feishu | `src/channels/feishu.c` | ✅ Active |
| feishu_ws | `src/channels/feishu_ws.c` | ✅ Active |

### External Channel Plugins (.so files)
| Channel | Plugin | Status |
|---------|--------|--------|
| telegram | `plugins/channels/telegram_channel/` | ✅ Ready |
| email | `plugins/channels/email_channel/` | ✅ Ready |
| discord | `plugins/channels/discord_channel/` | ✅ Ready |
| slack | `plugins/channels/slack_channel/` | ✅ Ready |
| dingtalk | `plugins/channels/dingtalk_channel/` | ✅ Ready |

### MCP Servers (configuration-driven)
MCP servers are loaded via `config.json`, not as .so plugins:
- **amap** (Gaode Maps): 7 geographic tools (geocoding, weather, POI search, etc.)

## Compilation

```bash
make              # Build for macOS
make android      # Build for Android (requires ANDROID_NDK)
make test         # Build and run unit tests
make clean        # Clean build artifacts
make package      # Create self-extracting installer
```

### Build All Plugins

```bash
cd plugins && make          # Build all external plugins
cd plugins && make install  # Install plugins to build/.primagen/plugins/external/
```

## Running

```bash
./build/primagen onboard
# Ensure API keys are set in .primagen/config.json

./build/primagen agent
```

The program runs an interactive CLI agent loop.

## Runtime Configuration

- **Config**: `.primagen/config.json` (API keys, model settings, channel config)
- **Memory**: `.primagen/memory/` (MEMORY.md, HISTORY.md)
- **Logs**: `.primagen/log/`
- **Sessions**: `.primagen/sessions/`
- **Skills**: `skills/` directory (built-in and custom)
- **MCP Servers**: `mcp-servers/` (e.g., amap for Gaode Maps integration)
- **Plugins**: `.primagen/plugins/external/` (dynamically loaded `.so` files)

## Notes

- **Memory**: Refers to the AI's "Memory" (Context/Knowledge), stored in `.primagen/memory/`.
- **Logs**: Application logs are stored in `.primagen/log/`.
- **Tools**: Includes `exec`, `read_file`, `write_file`, `web_search`, `skill`, `memory` etc.
- **MCP Tools**: External tools via Model Context Protocol (e.g., `amap_geocode`, `amap_weather`, `amap_search_place`).

---

## Plugin Development Guide

### Plugin Types

Primagen supports three types of plugins:

| Type | Description | Example |
|------|-------------|---------|
| **Tool** | Extend agent capabilities with new tools | `echo_tool`, `file_ops` |
| **Channel** | Add new communication channels | `telegram_channel`, `discord_channel` |
| **Command** | Add new slash commands | `hello_command`, `/deploy` |

### Plugin Structure

Every plugin must export these functions:

```c
// Return plugin information
PLUGIN_EXPORT PluginInfo* plugin_get_info(void);

// Called when plugin is loaded (return 0 on success)
PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context);

// Called when plugin is unloaded (return 0 on success)
PLUGIN_EXPORT int plugin_cleanup(void);
```

### Creating a Tool Plugin

**Example: `plugins/tools/my_tool/my_tool.c`**

```c
#include "../../../src/include/plugin.h"
#include "../../../src/plugin/plugin_manager.h"
#include "../../../src/include/logger.h"

// Tool implementation
static Error my_tool_execute(void* user_data, const char* args_json, String* result) {
    cJSON* args = cJSON_Parse(args_json);
    if (!args) {
        return error_new(ERR_JSON, "Invalid JSON arguments");
    }

    cJSON* message = cJSON_GetObjectItem(args, "message");
    const char* msg = message ? message->valuestring : "Hello";

    *result = string_new(msg);
    cJSON_Delete(args);
    return error_new(ERR_NONE, "");
}

// Plugin initialization
PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context) {
    (void)context;
    log_info("[MyTool] Initializing plugin");

    plugin_register_tool(manager, NULL, "my_tool",
        "My custom tool that processes messages",
        "{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\"}}}",
        my_tool_execute, NULL);

    return 0;
}

// Plugin cleanup
PLUGIN_EXPORT int plugin_cleanup(void) {
    log_info("[MyTool] Cleaning up plugin");
    return 0;
}

// Plugin info
static PluginInfo g_plugin_info = {
    .version = 1,
    .type = PLUGIN_TOOL,
    .name = "my_tool",
    .description = "My custom tool plugin",
    .plugin_id = "my_tool_plugin"
};

PLUGIN_EXPORT PluginInfo* plugin_get_info(void) {
    return &g_plugin_info;
}
```

**Makefile: `plugins/tools/my_tool/Makefile`**

```makefile
PLUGIN_NAME = my_tool
PLUGIN_SRC = my_tool.c
PLUGIN_SO = $(PLUGIN_NAME).so

PRIMAGEN_ROOT = ../../..
BUILD_DIR = $(PRIMAGEN_ROOT)/build

INCLUDES = -I$(PRIMAGEN_ROOT)/src/include \
           -I$(PRIMAGEN_ROOT)/src \
           -I$(PRIMAGEN_ROOT)/src/vendor

CC = gcc
CFLAGS = -fPIC -Wall -Wextra -std=c99 -g $(INCLUDES)

LDFLAGS = -shared -ldl -pthread
ifeq ($(shell uname -s),Darwin)
    LDFLAGS += -undefined dynamic_lookup
endif

all: $(BUILD_DIR) $(PLUGIN_SO)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(PLUGIN_SO): $(PLUGIN_SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install: $(PLUGIN_SO)
	@mkdir -p $(BUILD_DIR)/.primagen/plugins/external
	cp $(PLUGIN_SO) $(BUILD_DIR)/.primagen/plugins/external/
	@echo "Installed $(PLUGIN_SO)"

clean:
	rm -f $(PLUGIN_SO)

.PHONY: all install clean
```

### Creating a Channel Plugin

**Example: `plugins/channels/my_channel/my_channel.c`**

```c
#include "../../../src/include/channel.h"
#include "../../../src/include/logger.h"
#include "../../../src/plugin/plugin_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    MessageBus* bus;
    bool running;
} MyChannelData;

// Channel implementation
static bool my_channel_init(Channel* self, Config* cfg, MessageBus* bus) {
    MyChannelData* data = malloc(sizeof(MyChannelData));
    data->bus = bus;
    data->running = false;
    self->user_data = data;
    log_info("[MyChannel] Initialized");
    return true;
}

static void my_channel_start(Channel* self) {
    MyChannelData* data = (MyChannelData*)self->user_data;
    data->running = true;
    log_info("[MyChannel] Started");
}

static void my_channel_stop(Channel* self) {
    MyChannelData* data = (MyChannelData*)self->user_data;
    data->running = false;
    log_info("[MyChannel] Stopped");
}

static void my_channel_send(Channel* self, OutboundMessage* msg) {
    if (strcmp(msg->channel.data, "my_channel") != 0) return;
    printf("[MyChannel] %s: %s\n", msg->chat_id.data, msg->content.data);
}

static void my_channel_destroy(Channel* self) {
    if (self->user_data) free(self->user_data);
    log_info("[MyChannel] Destroyed");
    free(self);
}

// Channel factory
static Channel* my_channel_create(void) {
    Channel* ch = calloc(1, sizeof(Channel));
    ch->name = strdup("my_channel");
    ch->init = my_channel_init;
    ch->start = my_channel_start;
    ch->stop = my_channel_stop;
    ch->send = my_channel_send;
    ch->destroy = my_channel_destroy;
    return ch;
}

// Plugin initialization
PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context) {
    (void)context;
    log_info("[MyChannel] Initializing plugin");

    plugin_register_channel(manager, NULL, "my_channel", my_channel_create);
    return 0;
}

PLUGIN_EXPORT int plugin_cleanup(void) {
    log_info("[MyChannel] Cleaning up plugin");
    return 0;
}

static PluginInfo g_plugin_info = {
    .version = 1,
    .type = PLUGIN_CHANNEL,
    .name = "my_channel",
    .description = "My custom channel plugin",
    .plugin_id = "my_channel_plugin"
};

PLUGIN_EXPORT PluginInfo* plugin_get_info(void) {
    return &g_plugin_info;
}
```

### Building and Installing Plugins

```bash
# Build all plugins
cd plugins && make

# Build individual plugin
cd plugins/tools/my_tool && make

# Install all plugins
cd plugins && make install

# Verify installation
ls -la build/.primagen/plugins/external/
```

### Using Plugins

1. **Install plugins** to `.primagen/plugins/external/`
2. **Start Primagen** - plugins are loaded automatically on startup
3. **Or reload** at runtime with `/reload-plugins` command

---

## MCP Configuration Guide

MCP (Model Context Protocol) allows Primagen to use external tools via stdio transport.

### Configuration Structure

Add MCP servers to `.primagen/config.json`:

```json
{
  "mcp": {
    "enabled": true,
    "servers": [
      {
        "server_id": "amap",
        "transport_type": "stdio",
        "command": "node",
        "args": ["/path/to/amap-server/index.js"],
        "env": {
          "AMAP_API_KEY": "your_api_key_here"
        }
      }
    ]
  }
}
```

### Configuration Fields

| Field | Type | Description |
|-------|------|-------------|
| `enabled` | boolean | Enable/disable MCP entirely |
| `servers` | array | List of MCP server configurations |
| `server_id` | string | Unique identifier for this server |
| `transport_type` | string | Transport type: `"stdio"` (only supported type) |
| `command` | string | Command to execute (e.g., `node`, `python`) |
| `args` | array | Command arguments |
| `env` | object | Environment variables for the server |

### Example: Amap (Gaode Maps) Server

```json
{
  "mcp": {
    "enabled": true,
    "servers": [
      {
        "server_id": "amap",
        "transport_type": "stdio",
        "command": "node",
        "args": ["mcp-servers/amap/dist/index.js"],
        "env": {
          "AMAP_API_KEY": "YOUR_AMAP_API_KEY"
        }
      }
    ]
  }
}
```

### Available MCP Tools (Amap)

| Tool | Description |
|------|-------------|
| `amap_geocode` | Convert address to coordinates |
| `amap_weather` | Get weather for a location |
| `amap_search_place` | Search for places by keyword |
| `amap_place_detail` | Get place details by ID |
| `amap_distance` | Calculate distance between points |
| `amap_direction_driving` | Get driving directions |
| `amap_direction_walking` | Get walking directions |

### Using MCP Tools

Once configured, MCP tools are automatically available:

```
User: 北京今天天气怎么样？
Assistant: 我来查询北京的天气。
       [calls amap_weather with location="北京"]
       北京今天晴朗，温度 25°C。
```

---

## ACP (Agent Communication Protocol) Guide

ACP provides an HTTP API for external AI agents to interact with Primagen.

### Starting ACP Server

```bash
./build/primagen agent --acp-port 8080
```

### API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/v1/health` | GET | Health check |
| `/v1/tools/list` | GET | List available tools |
| `/v1/tools/call` | POST | Call a tool |
| `/v1/chat/completions` | POST | OpenAI-compatible chat endpoint |

### Examples

#### Health Check

```bash
curl http://localhost:8080/v1/health
```

Response:
```json
{"status":"ok","running":true}
```

#### List Tools

```bash
curl http://localhost:8080/v1/tools/list
```

Response:
```json
{
  "tools": [
    {"name": "read_file", "description": "...", "parameters": {...}},
    {"name": "write_file", "description": "...", "parameters": {...}},
    {"name": "amap_weather", "description": "...", "parameters": {...}}
  ]
}
```

#### Call Tool

```bash
curl -X POST http://localhost:8080/v1/tools/call \
  -H "Content-Type: application/json" \
  -d '{
    "tool_name": "read_file",
    "arguments": {"path": "test.txt"}
  }'
```

Response:
```json
{
  "tool_name": "read_file",
  "result": "File content here...",
  "error": null
}
```

#### Chat Completion (OpenAI-compatible)

```bash
curl -X POST http://localhost:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "primagen",
    "messages": [
      {"role": "system", "content": "You are a helpful assistant."},
      {"role": "user", "content": "Hello!"}
    ],
    "session_id": "session_123",
    "temperature": 0.7
  }'
```

Response:
```json
{
  "id": "chatcmpl-123",
  "object": "chat.completion",
  "created": 1234567890,
  "model": "primagen",
  "choices": [
    {
      "index": 0,
      "message": {"role": "assistant", "content": "Hello! How can I help you?"},
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 10,
    "completion_tokens": 8,
    "total_tokens": 18
  }
}
```

### Error Responses

```json
{
  "error": {
    "code": 400,
    "message": "Invalid request"
  }
}
```

| Code | Description |
|------|-------------|
| 400 | Bad request (invalid JSON, missing fields) |
| 404 | Endpoint not found |
| 405 | Method not allowed |
| 500 | Internal server error |
