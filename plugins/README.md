# Primagen Plugins

This directory contains demo plugins for the Primagen agent system.

## Plugin Types

Primagen supports three types of plugins:

1. **Tool Plugins** - Extend the agent's capabilities with new tools
2. **Channel Plugins** - Add new communication channels
3. **Command Plugins** - Add new slash commands

## Demo Plugins

| Plugin | Type | Description |
|--------|------|-------------|
| `tools/echo_tool` | Tool | Echoes back user messages with a prefix |
| `channels/stdout_channel` | Channel | Outputs messages to stdout with custom prefix |
| `commands/hello_command` | Command | Adds a `/hello` slash command |

## Building Plugins

### Build All Plugins

```bash
cd plugins
make all
```

### Build Individual Plugin

```bash
cd plugins/tools/echo_tool
make
```

### Install Plugins

```bash
cd plugins
make install
```

This copies all `.so` files to `build/.primagen/plugins/`.

## Using Plugins

1. **Build the plugins:**
   ```bash
   make all
   ```

2. **Install the plugins:**
   ```bash
   make install
   ```

3. **Run Primagen:**
   ```bash
   ./build/primagen agent
   ```

4. **Load plugins:**
   - External plugins in `.primagen/plugins/` are loaded automatically on startup
   - The plugin manager recursively scans subdirectories (channels/, commands/, tools/, etc.)
   - Or use the `/reload-plugins` command in the agent

## Creating Your Own Plugin

### Tool Plugin Example

```c
#include "../../../src/include/plugin.h"
#include "../../../src/plugin/plugin_manager.h"

static Error my_tool(void* user_data, const char* args_json, String* result) {
    *result = string_new("Hello from my tool!");
    return error_new(ERR_NONE, "");
}

PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context) {
    plugin_register_tool(manager, NULL, "my_tool",
        "My custom tool",
        "{\"type\":\"object\",\"properties\":{}}",
        my_tool, NULL);
    return 0;
}

PLUGIN_EXPORT int plugin_cleanup(void) {
    return 0;
}

static PluginInfo g_plugin_info = {
    .version = 1,
    .type = PLUGIN_TOOL,
    .name = "my_plugin",
    .description = "My custom plugin",
    .plugin_id = "my_plugin_id"
};

PLUGIN_EXPORT PluginInfo* plugin_get_info(void) {
    return &g_plugin_info;
}
```

### Plugin Makefile Template

```makefile
PLUGIN_NAME = my_plugin
PLUGIN_SRC = my_plugin.c
PLUGIN_SO = $(PLUGIN_NAME).so

PRIMAGEN_ROOT = ../../..

INCLUDES = -I$(PRIMAGEN_ROOT)/src/include \
           -I$(PRIMAGEN_ROOT)/src \
           -I$(PRIMAGEN_ROOT)/src/vendor

CC = gcc
CFLAGS = -fPIC -shared -Wall -Wextra -std=c99 -g $(INCLUDES)
LDFLAGS = -ldl

all: $(PLUGIN_SO)

$(PLUGIN_SO): $(PLUGIN_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(PLUGIN_SO)
	cp $(PLUGIN_SO) $(PRIMAGEN_ROOT)/build/.primagen/plugins/

clean:
	rm -f $(PLUGIN_SO)

.PHONY: all install clean
```

## Plugin API Reference

### Required Exports

Every plugin must export these functions:

```c
// Return plugin information
PluginInfo* plugin_get_info(void);

// Called when plugin is loaded (return 0 on success)
int plugin_init(PluginManager* manager, void* context);

// Called when plugin is unloaded (return 0 on success)
int plugin_cleanup(void);
```

### PluginInfo Structure

```c
typedef struct {
    int version;           // Plugin version
    PluginType type;       // PLUGIN_TOOL, PLUGIN_CHANNEL, PLUGIN_COMMAND
    const char* name;      // Plugin name
    const char* description; // Plugin description
    const char* plugin_id; // Unique identifier
    void* metadata;        // Type-specific metadata
} PluginInfo;
```

### Registration Functions

**Tools:**
```c
int plugin_register_tool(PluginManager* manager, LoadedPlugin* plugin,
                         const char* name, const char* desc,
                         const char* params, ToolExecuteFunc exec, void* user_data);
```

**Channels:**
```c
int plugin_register_channel(PluginManager* manager, LoadedPlugin* plugin,
                            const char* name, ChannelCreateFunc create);
```

**Commands:**
```c
int plugin_register_command(PluginManager* manager, LoadedPlugin* plugin,
                            const char* name, const char* desc, CommandFunc handler);
```

## Debugging Plugins

Enable verbose logging to see plugin loading:

```bash
./build/primagen agent --verbose
```

Check logs in `.primagen/log/` for plugin initialization messages.
