#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

#include <stddef.h>
#include <stdbool.h>
#include "../include/plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Plugin Manager Implementation Details
// =============================================================================

// Directory scanning helper
typedef struct {
    char** files;
    size_t count;
    size_t capacity;
} StringList;

// Internal functions (not exposed to plugins)
StringList* scan_directory(const char* dir_path, const char* extension);
void string_list_free(StringList* list);

LoadedPlugin* load_plugin_from_file(PluginManager* manager, const char* path);

// =============================================================================
// Plugin Registration (called by plugins during init)
// =============================================================================

/**
 * Register a tool from a plugin
 * Note: This function is called by plugin code during plugin_init
 * @param manager The plugin manager
 * @param plugin The plugin registering the tool (NULL for builtin)
 * @param name Tool name
 * @param desc Tool description
 * @param params JSON schema for parameters
 * @param exec Execute callback
 * @param user_data User data for tool
 * @return 0 on success, -1 on failure
 */
int plugin_register_tool(PluginManager* manager, LoadedPlugin* plugin,
                         const char* name, const char* desc,
                         const char* params, ToolExecuteFunc exec, void* user_data);

/**
 * Register a channel factory from a plugin
 * Note: This function is called by plugin code during plugin_init
 * @param manager The plugin manager
 * @param plugin The plugin registering the channel (NULL for builtin)
 * @param name Channel name
 * @param create Factory function
 * @return 0 on success, -1 on failure
 */
int plugin_register_channel(PluginManager* manager, LoadedPlugin* plugin,
                            const char* name, ChannelCreateFunc create);

/**
 * Register a command from a plugin
 * Note: This function is called by plugin code during plugin_init
 * @param manager The plugin manager
 * @param plugin The plugin registering the command (NULL for builtin)
 * @param name Command name (for /command)
 * @param desc Command description
 * @param handler Command handler function
 * @return 0 on success, -1 on failure
 */
int plugin_register_command(PluginManager* manager, LoadedPlugin* plugin,
                            const char* name, const char* desc, CommandFunc handler);

/**
 * Get the list of registered commands
 * @param manager The plugin manager
 * @param out_count Output parameter for the number of commands
 * @return Pointer to the internal commands array (do not modify or free)
 */
CommandPluginDef* plugin_manager_get_commands(PluginManager* manager, size_t* out_count);

/**
 * Get the list of registered channels
 * @param manager The plugin manager
 * @param out_count Output parameter for the number of channels
 * @return Pointer to the internal channels array (do not modify or free)
 */
const char** plugin_manager_get_channels(PluginManager* manager, size_t* out_count);

#ifdef __cplusplus
}
#endif

#endif // PLUGIN_MANAGER_H
