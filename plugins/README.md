# Plugins

Primagen extends its capabilities through a plugin system. Plugins are compiled as shared libraries (`.so`) and loaded dynamically at runtime via `dlopen`.

## Plugin Types

| Type        | Directory           | Purpose                     | Example                                                   |
| ----------- | ------------------- | --------------------------- | --------------------------------------------------------- |
| **Channel** | `plugins/channels/` | Add messaging backends      | `webui_channel`, `feishu_channel`, `dingtalk_channel`, `acp_channel` |
| **Tool**    | `plugins/tools/`    | Register LLM-callable tools | `web_tools`, `mcp_tools`, `echo_tool`                     |
| **Command** | `plugins/commands/` | Register CLI commands       | `hello_command`                                           |

## Available Plugins

### Channels (11)

| Plugin                                          | Description                                                            | Status   |
| ----------------------------------------------- | ---------------------------------------------------------------------- | -------- |
| [webui\_channel](channels/webui_channel/)       | Built-in WebUI with chat, control panel, skills manager, plugin upload | External |
| [acp\_channel](channels/acp_channel/)           | Agent Communication Protocol (MCP-compatible)                          | External |
| [feishu\_channel](channels/feishu_channel/)     | Feishu / Lark bot                                                      | External |
| [dingtalk\_channel](channels/dingtalk_channel/) | DingTalk bot                                                           | External |
| [telegram\_channel](channels/telegram_channel/) | Telegram bot                                                           | External |
| [slack\_channel](channels/slack_channel/)       | Slack bot                                                              | External |
| [discord\_channel](channels/discord_channel/)   | Discord bot                                                            | External |
| [email\_channel](channels/email_channel/)       | Email receiver (IMAP) + sender (SMTP)                                  | External |
| [stdout\_channel](channels/stdout_channel/)     | Pipe output to stdout for scripting                                    | External |
| [mqtt\_channel](channels/mqtt_channel/)         | MQTT publish/subscribe                                                 | External |

### Tools (3)

| Plugin                         | Description                               | Status   |
| ------------------------------ | ----------------------------------------- | -------- |
| [web\_tools](tools/web_tools/) | Web search + web fetch tools              | External |
| [mcp\_tools](tools/mcp_tools/) | MCP (Model Context Protocol) client tools | External |
| [echo\_tool](tools/echo_tool/) | Debug/test: echoes input                  | Example  |

### Commands (1)

| Plugin                                    | Description                           | Status  |
| ----------------------------------------- | ------------------------------------- | ------- |
| [hello\_command](commands/hello_command/) | Debug/test: prints "hello" to console | Example |

## Configuration

Plugins are configured in `.primagen/config.json`:

```jsonc
{
  "plugins": {
    "web_tools": { "enabled": false, "config": {} },
    "mcp_tools": { "enabled": false, "config": {} }
  }
}
```

Each plugin's `config` block may contain plugin-specific settings. See individual plugin READMEs for details.

## Loading Plugins

### From .primagen/plugins/ (auto-load)

Place compiled `.so` files in `.primagen/plugins/`. They are automatically loaded on startup if their name matches an `enabled: true` entry in `config.json.plugins`.

### Via WebUI Upload

Use the WebUI Control Panel → Plugins → Upload to add `.so` files at runtime. The plugin is saved to `.primagen/plugins/` and loaded immediately.

### Manual Load

```bash
cp my_plugin.so .primagen/plugins/
# Add to config.json:
# "plugins": { "my_plugin": { "enabled": true, "config": {} } }
```

## Plugin Development

### Minimal Channel Plugin

```c
#include "plugin.h"

PluginInfo plugin_info = {
    .type = PLUGIN_TYPE_CHANNEL,
    .name = "my_channel",
    .version = "1.0.0"
};

static void my_send(void* ctx, const char* key, const char* content) {
    // Send message to your messaging platform
}

static void my_close(void* ctx) {
    // Cleanup
}

static void my_thread(void* arg) {
    // Long-running loop: receive messages, call message_bus_send_inbound()
}

bool plugin_init(PluginManager* mgr, Config* cfg) {
    ChannelPlugin* ch = channel_plugin_new("my_channel", my_thread, my_send, NULL, my_close);
    return plugin_register_channel(mgr, ch, cfg);
}

void plugin_cleanup(PluginManager* mgr) {
    // Cleanup any resources
}
```

### Minimal Tool Plugin

```c
#include "plugin.h"

PluginInfo plugin_info = {
    .type = PLUGIN_TYPE_TOOL,
    .name = "my_tool",
    .version = "1.0.0"
};

static Error my_execute(void* user_data, const char* args_json, String* result) {
    // Parse args_json, do work, write result
    string_append(result, "{\"status\":\"ok\"}");
    return error_new(ERR_NONE, "");
}

bool plugin_init(PluginManager* mgr, Config* cfg) {
    return plugin_register_tool(mgr, NULL, "my_tool", "My custom tool",
        "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"}},\"required\":[\"input\"]}",
        my_execute, cfg);
}

void plugin_cleanup(PluginManager* mgr) {
    // Cleanup
}
```

### Build

```bash
# Build against the main binary
gcc -shared -fPIC -o my_plugin.so my_plugin.c \
    -I../src/include -I../src/vendor/cJSON -I../src/vendor/mongoose \
    -L../build -l:primagen
```

Each plugin directory in this repo has its own Makefile. Run `make` in the plugin directory:

```bash
cd plugins/tools/web_tools && make
```

## Plugin Lifecycle

1. **Load**: `plugin_manager_load_external()` calls `dlopen()` and `dlsym()` for `plugin_info`, `plugin_init`, `plugin_cleanup`
2. **Init**: `plugin_init(plugin_manager, config)` — register channels/tools/commands
3. **Run**: Channel threads start, tools are callable, commands are available
4. **Stop**: `plugin_cleanup(plugin_manager)` — cleanup resources
5. **Unload**: `dlclose()` the `.so` handle

## Troubleshooting

| Problem                   | Solution                                                                         |
| ------------------------- | -------------------------------------------------------------------------------- |
| `.so` fails to load       | Run `file my_plugin.so` to verify architecture; must match main binary           |
| `undefined symbol` errors | Plugin was built without linking against `libprimagen`; use `make` in plugin dir |
| Plugin not auto-loaded    | Verify `enabled: true` in config.json and `.so` exists in `.primagen/plugins/`   |
| Tool not appearing        | Check WebUI → Plugins → Tools tab; look for errors in `agent.log`                |
| Channel thread crashes    | Check `agent.log` for crash backtraces; ensure thread is properly initialized    |

