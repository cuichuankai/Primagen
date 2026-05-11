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

Primagen is a lightweight, high-performance AI Agent runtime written in pure C99. It connects LLMs to the real world through a plugin-based architecture, supporting multiple messaging channels, built-in tools, skills, cron scheduling, subagents, and long-term memory — all running as a single native binary with minimal resource footprint.

---
## What Primagen Provides

- Message-driven Agent Loop with tool calling, and state machine per session to manage the conversation flow.
- Thread-safe message bus between channels and core runtime
- Dynamic plugin system for channels, tools, and commands
- Session persistence (`.primagen/sessions`)
- Dual-layer Memory Storage (MEMORY.md + HISTORY.md)
- MCP client integration for external tool servers (Supports 4 transports: stdio / SSE / WebSocket / Streamable HTTP)
- Optional ACP server with OpenAI-compatible endpoints

## Quick Start

```bash
# 1. Build
make

# 2. Configure
cp config/config.example.json .primagen/config.json
# Edit .primagen/config.json: set your model, API key, and API base

# 3. Run
./build/primagen agent
```

Open `http://localhost:8090` for the WebUI chat console.

---

## Architecture

```
Inbound Channels → Message Bus (inbound) → Agent Loop (State Machine)
                                                   │
                              ┌────────────────────┼────────────────────┐
                              ▼                    ▼                    ▼
                         LLM Provider          Tool Executor        Cron Service
                         (Mongoose async)     (fork/exec)          (schedule)
                              │                    │                    │
                              └────────────────────┼────────────────────┘
                                                   ▼
                                           Message Bus (internal)
                                                   │
                              ┌────────────────────┼────────────────────┐
                              ▼                    ▼                    ▼
                         Session Manager      Context Builder      Subagent Mgr
                         (JSONL persist)      (Memory/Skills)      (nested agent)
                              │
                              ▼
                         Message Bus (outbound) → Outbound Channels
```

### Core Modules

| Module | Description |
|--------|-------------|
| **Agent Loop** | State machine (`IDLE → WAITING_LLM → WAITING_TOOL → IDLE`), 15 max tool iterations |
| **Message Bus** | Three-queue system: `inbound` / `internal` / `outbound`, thread-safe |
| **Session Manager** | Per-channel:chat_id isolation, JSONL persistence, RWLock hash table |
| **LLM Provider** | Async HTTP client via Mongoose, SSE streaming, OpenAI-compatible API |
| **Tool Executor** | Fork/exec model with timeout, async callback via internal events |
| **Context Builder** | Assembles system prompt from memory (MEMORY.md), skills, and tools |
| **Memory System** | Two-tier: MEMORY.md (long-term facts) + HISTORY.md (summarized history) |
| **Cron Service** | Scheduler for one-shot and recurring tasks, `pthread_cond_timedwait` driven |
| **Subagent Manager** | Spawns nested agents for parallel/delegated tasks with subset tools |
| **Plugin Manager** | Dynamic `.so` loading via `dlopen`, supports channels/tools/commands |
| **Skills Loader** | Loads SKILL.md modules from local filesystem, SkyPilot, or ClawHub |

### State Machine

AgentLoop implements a state machine per session to manage the conversation flow:

```text
                    +-------+
                    | IDLE  |<------------------+
                    +-------+                   |
                        |                       |
           (new message)|                       |(no tool_calls)
                        v                       |
                +---------------+               |
                | WAITING_LLM   |---------------+
                +---------------+               |
                        |                       |
            (tool_calls returned)               |
                        v                       |
                +---------------+               |
                | WAITING_TOOL  |---------------+
                +---------------+   (tool done, next turn)
```

**States**:

| State | Description |
|-------|-------------|
| `IDLE` | Session is ready to accept new messages |
| `WAITING_LLM` | LLM request in progress |
| `WAITING_TOOL` | Tool execution in progress |
---
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
---
## Built-in Tools

These tools are registered automatically and require no plugins:

| Tool | Description | Parameters |
|------|-------------|------------|
| `read_file` | Read file contents | `path` |
| `write_file` | Create or overwrite a file | `path`, `content` |
| `edit_file` | Search-and-replace within a file | `path`, `old_str`, `new_str` |
| `list_dir` | List directory contents | `path` |
| `exec` | Execute a shell command | `command` |
| `send_message` | Send message with optional attachments | `content`, `attachments[]` |
| `spawn_subagent` | Delegate a task to a sub-agent | `task`, `label` |
| `cron` | Schedule one-shot or recurring tasks | `name`, `payload`, `schedule`, `channel`, `chat_id` |
| `skill` | Load/unload/list skills | `action` (`list`/`load`/`unload`), `name` |
| `memory` | Manage long-term memory | `history_entry`, `memory_update`, `content` |

**Subagent Tools** (limited set available to spawned sub-agents):
`read_file`, `write_file`, `edit_file`, `list_dir`, `exec`, `send_message`

---

## Skills System

Skills are modular prompt extensions loaded from `.md` files. The agent loads them on demand via the `skill` tool, injecting their content into the system prompt.

**Sources** (configured in `config.json`):
- **Local**: `skills/` directory in the installation path
- **SkyPilot**: Community skills from sky-pilot/skills (GitHub)
- **ClawHub**: Community skills from claudehub.com

**Example SKILL.md**:
```markdown
# Skill: code-review

You are a code review expert. When reviewing code, focus on:
1. Security vulnerabilities
2. Performance bottlenecks
3. Code style consistency
4. Error handling completeness

Always provide specific line references and actionable suggestions.
```

Skills are managed via the WebUI Control Panel → Skills tab, or programmatically with the `skill` tool.

---

## Memory System

Primagen uses a two-tier memory model to maintain context across conversations:

| Tier | File | Purpose |
|------|------|---------|
| **Long-term Facts** | `.primagen/workspace/memory/MEMORY.md` | Key facts, preferences, decisions — persists across all sessions |
| **History Summary** | `.primagen/workspace/memory/HISTORY.md` | Summarized conversation history, auto-compressed when nearing token limits |

**Consolidation Trigger**: When the combined context exceeds `memory_window` tokens (configurable, default 100000), older messages are compressed into HISTORY.md and key facts are extracted into MEMORY.md.

**Safety**: The `memory_update` parameter is a **complete replacement**. The agent must include ALL existing facts when updating — missing facts are permanently deleted. Use `content` parameter for safe incremental additions.

---

## Subagent System

Spawn isolated sub-agents to handle parallel or delegated tasks:

- **Trigger**: Agent calls `spawn_subagent` tool with a task description
- **Tools**: Sub-agents have a restricted tool set (file ops + exec + send_message)
- **Isolation**: Each sub-agent has its own session, status tracking, and max iterations
- **Status**: Monitor via WebUI Control Panel → Subagents tab

---

## WebUI Control Panel

The Primagen's WebUI Control Panel at `http://localhost:8090` provides:

| Tab | Features |
|-----|----------|
| **Chat** | Live conversation with the agent, image uploads, file attachments |
| **Plugins** | View loaded channels/tools/commands, enable/disable, upload `.so` files |
| **Skills** | Browse, load, import, and edit skills content |
| **Sessions** | List all active sessions with message counts, view conversations |
| **Usage** | Token usage statistics and cost tracking |
| **Cron** | View and manage scheduled cron jobs |
| **Config** | Read-only view of the current configuration |

### API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/health` | Health check, returns agent status |
| `POST` | `/api/chat/send` | Send a chat message |
| `GET` | `/api/chat/poll` | Long-poll for new replies (30s timeout) |
| `GET` | `/api/config` | Current configuration |
| `GET` | `/api/control/channels` | List loaded channel plugins |
| `GET` | `/api/control/tools` | List loaded tool plugins |
| `GET` | `/api/control/commands` | List loaded command plugins |
| `POST` | `/api/control/plugin-enable` | Enable/disable a plugin |
| `POST` | `/api/control/plugins/upload` | Upload a `.so` plugin |
| `GET` | `/api/control/skills` | List available skills |
| `GET` | `/api/control/skills/content` | Get skill content |
| `POST` | `/api/control/skills/save` | Save skill content |
| `POST` | `/api/control/skills/import` | Import skill from URL |
| `GET` | `/api/control/sessions` | List active sessions |
| `GET` | `/api/control/usage` | Token usage statistics |
| `GET` | `/api/control/cron` | List cron jobs |
| `GET` | `/api/settings/logs` | Recent log entries |
| `GET` | `/api/settings/docs` | Documentation content |

---

## Channels

Primagen supports multiple communication channels concurrently:

| Channel | Description | Key |
|---------|-------------|-----|
| `console` | Built-in CLI stdin/stdout | `console` |
| `webui` | Web chat UI with control panel | `webui` |
| `feishu` | Feishu/Lark bot | `feishu` |
| `dingtalk` | DingTalk bot | `dingtalk` |
| `telegram` | Telegram bot | `telegram` |
| `slack` | Slack bot | `slack` |
| `discord` | Discord bot | `discord` |
| `email` | Email (IMAP receiver + SMTP sender) | `email` |
| `stdout` | Pipe output to stdout (for scripting) | `stdout` |
| `mqtt` | MQTT messaging | `mqtt` |
| `acp` | Agent Communication Protocol (MCP-compatible) | `acp` |

Channels are configured in `config.json` under `channels`. Each channel has its own `enabled` flag and channel-specific configuration. See [plugins/README.md](plugins/README.md) for per-channel setup.

---

## Configuration

Primagen uses a layered configuration system:

1. **File**: `.primagen/config.json` (default values)
2. **Environment Variables**: Override file values at runtime

### Key Configuration Fields

```jsonc
{
  "agent": {
    "model": "gpt-4o",           // LLM model name
    "api_base": "https://...",     // API endpoint
    "api_key": "",                  // API key (or use OPENAI_API_KEY env)
    "temperature": 0.2,
    "max_tokens": 8000,
    "max_tool_iterations": 15,
    "memory_window": 100000,        // Token threshold for consolidation
    "reasoning_effort": "low"      // low/medium/high
  },
  "tools": {
    "restrict_to_workspace": true,  // Sandbox file operations
    "exec": {
      "timeout": 60,                // Command timeout (seconds)
      "path_append": "/usr/local/bin"
    }
  },
  "skills": {
    "sources": {
      "sky_pilot": { "enabled": false },
      "claw_hub": { "enabled": false }
    }
  },
  "plugins": {
    "web_tools": { "enabled": false, "config": {} },
    "mcp_tools": { "enabled": false, "config": {} }
  }
}
```

### Environment Variable Overrides

All config values can be set via environment variables:

```
PRIMAGEN_AGENT_MODEL                    Model name
PRIMAGEN_AGENT_API_KEY                  API key
PRIMAGEN_AGENT_API_BASE                 API base URL
PRIMAGEN_AGENT_TEMPERATURE             Temperature (0.0-2.0)
PRIMAGEN_AGENT_MAX_TOKENS              Max response tokens
PRIMAGEN_AGENT_MAX_TOOL_ITERATIONS     Max tool call rounds
PRIMAGEN_AGENT_MEMORY_WINDOW           Token window for consolidation
PRIMAGEN_AGENT_REASONING_EFFORT        Reasoning effort level

PRIMAGEN_TOOLS_RESTRICT_TO_WORKSPACE   Sandbox file ops (true/false)
PRIMAGEN_TOOLS_EXEC_TIMEOUT            Command timeout in seconds
PRIMAGEN_TOOLS_EXEC_RESTRICT_TO_WORKSPACE  Restrict exec to workspace
PRIMAGEN_TOOLS_EXEC_PATH_APPEND        Extra PATH entries
PRIMAGEN_TOOLS_WEB_PROXY               HTTP proxy for web tools
PRIMAGEN_TOOLS_WEB_SEARCH_API_KEY      Search API key

PRIMAGEN_TLS_SKIP_VERIFY              Skip TLS verification (dev only!)

PRIMAGEN_DNS_USE_SYSTEM_RESOLVER       Use system DNS instead of built-in

PRIMAGEN_HEARTBEAT_ENABLED            Enable heartbeat
PRIMAGEN_HEARTBEAT_INTERVAL_S         Heartbeat interval (seconds)

PRIMAGEN_LOG_LEVEL                    Log level: debug/info/warn/error
PRIMAGEN_LOG_CONSOLE_OUTPUT          Console output (true/false)

OPENAI_API_KEY                         Fallback if PRIMAGEN_AGENT_API_KEY not set
```

---

## Data Directory

All runtime data lives under `.primagen/`:

```
.primagen/
├── config.json              # Main configuration
├── sessions/                # Per-session JSONL files (session_{key}.jsonl)
├── workspace/
│   ├── memory/
│   │   ├── MEMORY.md        # Long-term facts
│   │   └── HISTORY.md       # Summarized history
│   └── files/               # AI-created files (when restrict_to_workspace=true)
├── skills/                  # Locally installed skills
│   └── {skill-name}/
│       └── SKILL.md
├── plugins/                 # External .so plugins
├── cron_jobs.json           # Persistent cron job definitions
├── heartbeat.json           # Heartbeat state
├── token_usage.json         # Usage tracking
└── agent.log                # Application and channel logs
```

---

## Building

```bash
# Standard build
make

# Clean build
make clean && make

# Build with debug symbols
make debug

# Build plugins only
make plugins

# Create self-extracting package
make package

# Cross-compile for Android (Termux)
make android
```

**Dependencies** (all vendored):
- [Mongoose](https://github.com/cesanta/mongoose) — HTTP/TCP network library
- [cJSON](https://github.com/DaveGamble/cJSON) — JSON parser
- [Jasmine](https://github.com/sheredom/jasmine) — Unit test framework

**System requirements**:
- GCC or Clang (C99)
- POSIX threads (pthread)
- `dlopen` support (for plugins)
- macOS, Linux, or Android (Termux)

---

## Plugin Development

Primagen supports three plugin types loaded as `.so` shared libraries:

| Type | Interface | Purpose |
|------|-----------|---------|
| **Channel** | `ChannelPlugin` | Add new messaging channels |
| **Tool** | `ToolPlugin` | Register custom LLM tools |
| **Command** | `CommandPlugin` | Register CLI commands |

Minimal plugin structure:
```c
#include "plugin.h"

PluginInfo plugin_info = { .type = PLUGIN_TYPE_CHANNEL, .name = "my_channel", .version = "1.0.0" };

bool plugin_init(PluginManager* mgr, Config* cfg) {
    ChannelPlugin* ch = channel_plugin_new("my_channel", my_channel_open, my_channel_send, NULL, my_channel_close);
    plugin_register_channel(mgr, ch, cfg);
    return true;
}

void plugin_cleanup(PluginManager* mgr) { /* cleanup */ }
```

See [plugins/README.md](plugins/README.md) for the complete development guide.

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| **API connection fails** | Set `PRIMAGEN_TLS_SKIP_VERIFY=1` for self-signed certs; verify API base URL ends with `/v1` |
| **Plugin .so fails to load** | Check `LD_LIBRARY_PATH`; verify plugin was compiled with same architecture as main binary |
| **Tool execution hangs** | Increase `PRIMAGEN_TOOLS_EXEC_TIMEOUT`; check for shell aliases in `.bashrc` |
| **Memory consolidation loses data** | Check `agent.log` for consolidation errors; increase `memory_window` if too frequent |
| **WebUI not accessible** | Default port is 8090; check `PRIMAGEN_WEBUI_PORT` in channel config |
| **Session file grows too large** | Consolidation is automatic at `memory_window` threshold; manually trigger via the `memory` tool |
| **High CPU usage** | Check cron jobs for very short intervals; WebUI now uses long polling (minimal idle CPU) |

---

## License

MIT License — see LICENSE file.