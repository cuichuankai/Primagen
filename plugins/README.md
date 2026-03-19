# Primagen Plugins

This directory contains external plugins for Primagen.

Primagen loads shared libraries (`.so`) recursively from:

```text
.primagen/plugins/
```

Subdirectories are grouped by plugin type:

```text
.primagen/plugins/channels/
.primagen/plugins/tools/
.primagen/plugins/commands/
```

## Plugin Types

Primagen supports three plugin categories:

1. **Channel plugins**: transport and message IO
2. **Tool plugins**: new callable tools for the agent
3. **Command plugins**: slash-style command handlers

## Current Plugins in This Repository

### Channels

- `channels/feishu_channel`
- `channels/dingtalk_channel`
- `channels/telegram_channel`
- `channels/discord_channel`
- `channels/slack_channel`
- `channels/email_channel`
- `channels/stdout_channel`

### Tools

- `tools/echo_tool`

### Commands

- `commands/hello_command`

## Build Workflow

Build all plugins:

```bash
cd plugins
make
```

Install all plugins into runtime plugin directories:

```bash
cd plugins
make install
```

Build a single plugin:

```bash
cd plugins/channels/feishu_channel
make
make install
```

## Output and Install Paths

Compile output:

```text
build/.primagen/plugins/<type>/<plugin>.so
```

Runtime install target:

```text
.primagen/plugins/<type>/<plugin>.so
```

## Plugin Loading Behavior

- Plugins are loaded automatically at startup.
- Loader scans `.primagen/plugins/` recursively.
- A plugin can skip registration if it is disabled by config.

## Required Plugin Exports

Every plugin shared library must export:

```c
PLUGIN_EXPORT PluginInfo* plugin_get_info(void);
PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context);
PLUGIN_EXPORT int plugin_cleanup(void);
```

## Minimal PluginInfo Example

```c
static PluginInfo g_plugin_info = {
    .version = 1,
    .type = PLUGIN_TOOL,
    .name = "my_plugin",
    .description = "My plugin",
    .plugin_id = "my_plugin_id"
};
```

## Development Notes

- Use `plugins/common.mk` in plugin Makefiles.
- Keep plugin IDs stable, because config lookup uses `plugin_id`.
- Register channels/tools/commands only inside `plugin_init`.

## Related Docs

- Root overview: `../README.md`
- Feishu plugin guide: `channels/feishu_channel/README.md`
- DingTalk plugin guide: `channels/dingtalk_channel/README.md`
