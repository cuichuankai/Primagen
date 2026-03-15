# Primagen C Project Structure

## Directory Organization

### Root Directory (/)

```
/workspaces/Primagen/
├── Makefile              # Compilation configuration
├── README.md             # Project documentation
├── PROJECT_STRUCTURE.md  # This file
├── IMPLEMENTATION_GUIDE.md # Implementation status and guide
├── docs/                 # Documentation directory
│   ├── AMAP_MCP_TEST_REPORT.md      # Amap MCP integration test report
│   └── AMAP_MCP_SETUP_REPORT.md     # Amap MCP setup guide
├── mcp-servers/          # MCP Server implementations
│   └── amap/             # Amap (Gaode Map) MCP server
├── src/                  # Source code directory
└── build/                # Build output directory (artifacts and executable)
```

### Source Code Directory (src/)

```
src/
├── include/              # Header directory (Common headers)
│   ├── channel.h         # Channel interface
│   ├── commands.h        # CLI commands interface
│   ├── common.h          # Common data structures and utilities
│   ├── config.h          # Configuration structures
│   ├── cron.h            # Cron service interface
│   ├── heartbeat.h       # Heartbeat service interface
│   ├── logger.h          # Logger interface
│   ├── message.h         # Message data structures
│   ├── skills.h          # Skills loader interface
│   └── subagent.h        # Subagent manager interface
├── agent/                # Agent Loop Module
│   ├── agent_loop.h
│   └── agent_loop.c
├── bus/                  # Message Bus Module
│   ├── message_bus.h
│   └── message_bus.c
├── channels/             # Communication Channels Module
│   ├── console.c         # CLI Console channel
│   ├── dingtalk.c
│   ├── discord.c
│   ├── email.c
│   ├── feishu.c
│   ├── feishu_ws.c       # Feishu WebSocket implementation
│   ├── feishu_ws.h
│   ├── slack.c
│   └── telegram.c
├── cli/                  # CLI Commands Module
│   └── commands.c
├── common/               # Common Implementations
│   ├── common.c          # Common utilities
│   ├── logger.c          # Logging implementation
│   └── message.c         # Message handling implementation
├── config/               # Configuration Module
│   └── config.c
├── context/              # Context Builder Module
│   ├── context_builder.h
│   └── context_builder.c
├── cron/                 # Cron Service Module
│   └── cron.c
├── heartbeat/            # Heartbeat Service Module
│   └── heartbeat.c
├── mcp/                  # MCP (Model Context Protocol) Module
│   ├── mcp.h             # MCP client and manager interface
│   ├── mcp.c             # MCP manager implementation
│   ├── mcp_tools.c       # MCP tools registration bridge
│   ├── mcp_tools.h       # MCP tools registration header
│   ├── transport_stdio.c # stdio transport implementation
│   └── transport_stdio.h # stdio transport header
├── memory/               # Memory Management Module
│   ├── memory.h
│   └── memory.c
├── providers/            # LLM Providers Module
│   ├── llm_provider.h
│   └── llm_provider.c
├── session/              # Session Management Module
│   ├── session.h
│   └── session.c
├── skills/               # Skills Management Module
│   └── skills.c
├── subagent/             # Subagent Management Module
│   └── subagent.c
├── tools/                # Tools Registry & Implementation
│   ├── tool.h            # Tool registry interface
│   ├── tool.c            # Tool registry implementation
│   ├── tools_impl.h      # Concrete tools declaration
│   └── tools_impl.c      # Concrete tools implementation (fs, shell, web, etc.)
├── vendor/               # Third-party Libraries
│   ├── cJSON/
│   │   ├── cJSON.h
│   │   └── cJSON.c
│   └── mongoose/
│       ├── mongoose.h
│       └── mongoose.c
└── main.c                # Main Entry Point
```

### Build Output Directory (build/)

```
build/
├── primagen               # Final Executable
├── common/
│   ├── common.o
│   ├── logger.o
│   └── message.o
├── main.o
├── agent/
│   └── agent_loop.o
├── bus/
│   └── message_bus.o
├── channels/
│   ├── console.o
│   ├── ... (other channels)
├── cli/
│   └── commands.o
├── config/
│   └── config.o
├── context/
│   └── context_builder.o
├── cron/
│   └── cron.o
├── heartbeat/
│   └── heartbeat.o
├── mcp/
│   ├── mcp.o
│   ├── mcp_tools.o
│   └── transport_stdio.o
├── memory/
│   └── memory.o
├── providers/
│   └── llm_provider.o
├── session/
│   └── session.o
├── skills/
│   └── skills.o
├── subagent/
│   └── subagent.o
├── tools/
│   ├── tool.o
│   └── tools_impl.o
└── vendor/
    └── cJSON/
        └── cJSON.o
    └── mongoose/
        └── mongoose.o
```

## Compilation

### From Root Directory:

```bash
make              # Compile project
make clean        # Clean build artifacts
```

## Running

```bash
./build/primagen
```

### Workspace Structure (.primagen/)

When running, the application creates/uses a workspace directory (default `.primagen`):

```
.primagen/
├── config.json           # Configuration file
├── cron_store.json       # Persisted cron jobs
├── log/
│   └── primagen.log      # Application logs
├── memory/
│   ├── MEMORY.md         # Long-term memory facts
│   └── HISTORY.md        # Consolidated conversation history
├── sessions/
│   └── console:local_user.jsonl  # Session history (JSONL format)
├── skills/               # Skills directory (loaded by SkillsLoader)
│   └── ...
├── IDENTITY.md           # Agent Identity
├── AGENTS.md             # Sub-agents definition
├── SOUL.md               # Core directives
├── USER.md               # User profile
└── TOOLS.md              # Tools documentation
```

## Project Features

1. **Modular Design**: Code separated by function (agent, bus, context, memory, mcp, etc.).
2. **Header Organization**: Common headers in `src/include/`.
3. **Clean Separation**: Implementation details hidden in `.c` files.
4. **ReAct Loop**: Full implementation of Reasoning + Acting loop in C.
5. **Persistent Memory**: File-based long-term memory (`MEMORY.md`).
6. **Tool System**: Extensible tool registration system with MCP support.
7. **MCP Integration**: Model Context Protocol client for external tools (stdio transport).
8. **Real-world Integration**:
   - **LLM**: libcurl integration with OpenAI/Brave.
   - **Channels**: Architecture supports multiple channels (Telegram, Feishu, etc.).
   - **Cron**: Persistent scheduling.
   - **MCP Servers**: External tool integration via MCP (e.g., Amap geographic services).

## Core Module Functions

| Module      | Function                                                               |
| ----------- | ---------------------------------------------------------------------- |
| `agent`     | Implements the main ReAct loop, handling multi-turn tool execution.    |
| `bus`       | Asynchronous message bus for decoupling components.                    |
| `channels`  | Implementation of various communication channels.                      |
| `config`    | Loads configuration from JSON files with environment variable support. |
| `context`   | Builds the system prompt from identity, memory, skills, and history.   |
| `cron`      | Manages scheduled tasks with persistent storage.                       |
| `mcp`       | MCP client for integrating external tools via Model Context Protocol.  |
| `memory`    | Manages long-term (facts) and short-term (history) memory.             |
| `providers` | Interface for LLM API calls (OpenAI compatible).                       |
| `session`   | Manages active sessions and their persistence.                         |
| `skills`    | Loads and manages dynamic skills from the filesystem.                  |
| `subagent`  | Manages spawning of sub-agents for tasks.                              |
| `tools`     | Registry and implementation of all agent capabilities (tools).         |

## MCP Integration

### MCP Module (`src/mcp/`)

| File                | Description                                           |
| ------------------- | ----------------------------------------------------- |
| `mcp.h`             | MCP client and manager interface                      |
| `mcp.c`             | MCP manager implementation with connection management |
| `mcp_tools.c`       | Bridge for registering MCP tools with ToolRegistry    |
| `transport_stdio.c` | stdio transport using child processes                 |

