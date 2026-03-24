#ifndef PLUGIN_H
#define PLUGIN_H

#include "common.h"
#include "../tools/tool.h"
#include "channel.h"
#include "config.h"
#include "../bus/message_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Plugin System Types
// =============================================================================

typedef enum {
    PLUGIN_TOOL,
    PLUGIN_CHANNEL,
    PLUGIN_MCP,
    PLUGIN_COMMAND
} PluginType;

// Forward declarations
struct PluginManager;
typedef struct CommandContext CommandContext;
typedef struct CommandPluginDef CommandPluginDef;

// Channel forward declarations (defined in channel.h)
// Channel and ChannelCreateFunc are defined in channel.h

// =============================================================================
// Tool Plugin Interface
// =============================================================================

typedef struct {
    char* name;
    char* description;
    char* parameters_json;  // JSON schema for parameters
    ToolExecuteFunc execute;
    void* user_data;
} ToolPluginDef;

// =============================================================================
// Channel Plugin Interface
// =============================================================================

// ChannelCreateFunc is forward-declared in channel.h
// ChannelPluginDef is defined in channel.h to avoid circular dependencies

// =============================================================================
// Command Plugin Interface
// =============================================================================

struct CommandContext {
    void* user_data;
    int (*send_response)(CommandContext* ctx, const char* message);
    int (*stop_active_tasks)(CommandContext* ctx);
    int (*reset_session)(CommandContext* ctx);
    CommandPluginDef* (*get_registered_commands)(CommandContext* ctx, size_t* out_count);
    ToolRegistry* (*get_tool_registry)(CommandContext* ctx);
    struct PluginManager* (*get_plugin_manager)(CommandContext* ctx);
};

typedef int (*CommandFunc)(CommandContext* ctx, Config* cfg, const char* workspace_path, int argc, char** argv);

struct CommandPluginDef {
    char* name;
    char* description;
    CommandFunc handler;
};

// =============================================================================
// Plugin Information Structure
// =============================================================================

typedef struct {
    int version;
    PluginType type;
    const char* name;
    const char* description;
    const char* plugin_id;  // Unique identifier
    void* metadata;  // Type-specific (ToolPluginDef*, ChannelPluginDef*, etc.)
} PluginInfo;

// =============================================================================
// Plugin Entry Points (exported from .so files)
// =============================================================================

typedef PluginInfo* (*PluginGetInfoFunc)(void);
typedef int (*PluginInitFunc)(struct PluginManager* manager, void* context);
typedef int (*PluginCleanupFunc)(void);

// =============================================================================
// Loaded Plugin Handle
// =============================================================================

typedef struct LoadedPlugin {
    char* id;
    char* name;
    char* path;
    PluginType type;
    void* handle;  // dlopen handle
    PluginInfo* info;
    PluginInitFunc init;
    PluginCleanupFunc cleanup;
    struct LoadedPlugin* next;
} LoadedPlugin;

// =============================================================================
// Plugin Manager
// =============================================================================

typedef struct PluginManager {
    LoadedPlugin* plugins;
    size_t plugin_count;

    // Plugin directories
    char* builtin_dir;
    char* external_dir;

    // Context references
    Config* config;
    MessageBus* bus;
    ToolRegistry* tool_registry;
    void* channel_array;  // Forward decl - actual type in main.c
    int* channel_count_ptr;
    int channel_capacity;

    // Command registration
    CommandPluginDef* commands;
    size_t command_count;
    size_t command_capacity;

    pthread_mutex_t lock;
} PluginManager;

// =============================================================================
// Plugin Manager Functions
// =============================================================================

PluginManager* plugin_manager_new(const char* base_dir);
void plugin_manager_free(PluginManager* manager);

// Plugin loading
int plugin_manager_load_external(PluginManager* manager);
int plugin_manager_load_plugin(PluginManager* manager, const char* path);
int plugin_manager_reload_plugin(PluginManager* manager, const char* plugin_id);

// Plugin discovery
LoadedPlugin* plugin_manager_find_plugin(PluginManager* manager, const char* plugin_id);
LoadedPlugin* plugin_manager_find_plugin_by_name(PluginManager* manager, const char* name);
void plugin_manager_free_plugin_snapshot(LoadedPlugin* plugin);

// Registration helpers (for plugins to register themselves)
int plugin_manager_register_tool(PluginManager* manager, ToolPluginDef* tool_def);
int plugin_manager_register_channel(PluginManager* manager, struct ChannelPluginDef* channel_def);
int plugin_manager_register_command(PluginManager* manager, CommandPluginDef* cmd_def);

// Plugin reloading support
void plugin_manager_unload_plugin(PluginManager* manager, LoadedPlugin* plugin);

// =============================================================================
// Plugin Helper Macros
// =============================================================================

// Macro to define plugin info for tool plugins
#define DEFINE_TOOL_PLUGIN(name, desc, params, exec_fn, user_data) \
    static ToolPluginDef tool_def_##name = { \
        .name = #name, \
        .description = desc, \
        .parameters_json = params, \
        .execute = exec_fn, \
        .user_data = user_data \
    }; \
    static PluginInfo plugin_info = { \
        .version = 1, \
        .type = PLUGIN_TOOL, \
        .name = #name, \
        .description = desc, \
        .plugin_id = "tool_" #name, \
        .metadata = &tool_def_##name \
    }; \
    PluginInfo* plugin_get_info(void) { return &plugin_info; }

// Macro to define plugin info for channel plugins
#define DEFINE_CHANNEL_PLUGIN(name, desc, create_fn) \
    static ChannelPluginDef channel_def_##name = { \
        .name = #name, \
        .create = create_fn \
    }; \
    static PluginInfo plugin_info = { \
        .version = 1, \
        .type = PLUGIN_CHANNEL, \
        .name = #name, \
        .description = desc, \
        .plugin_id = "channel_" #name, \
        .metadata = &channel_def_##name \
    }; \
    PluginInfo* plugin_get_info(void) { return &plugin_info; }

// Macro to define plugin info for command plugins
#define DEFINE_COMMAND_PLUGIN(name, desc, handler_fn) \
    static CommandPluginDef cmd_def_##name = { \
        .name = #name, \
        .description = desc, \
        .handler = handler_fn \
    }; \
    static PluginInfo plugin_info = { \
        .version = 1, \
        .type = PLUGIN_COMMAND, \
        .name = #name, \
        .description = desc, \
        .plugin_id = "command_" #name, \
        .metadata = &cmd_def_##name \
    }; \
    PluginInfo* plugin_get_info(void) { return &plugin_info; }

// Export macro for plugin entry points
#ifdef _WIN32
    #define PLUGIN_EXPORT __declspec(dllexport)
#else
    #define PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
}
#endif

#endif // PLUGIN_H
