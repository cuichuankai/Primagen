#include "plugin_manager.h"
#include "../include/logger.h"
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// =============================================================================
// Internal Helper Functions
// =============================================================================

// Cleanup a loaded plugin (called internally by plugin_manager_free)
static void plugin_manager_cleanup_plugin(LoadedPlugin* plugin) {
    if (!plugin) return;

    // Call plugin's cleanup function if it has one
    if (plugin->cleanup) {
        plugin->cleanup();
    }

    // Close the shared library handle
    if (plugin->handle) {
        dlclose(plugin->handle);
    }
}

// Forward declaration for recursive scanning
static void scan_directory_recursive(const char* dir_path, const char* extension, StringList* list);
static const PluginConfig* plugin_config_from_so_name(const Config* cfg, const char* path);

// Helper function to add a file to the string list
static void string_list_add(StringList* list, const char* path) {
    if (list->count >= list->capacity) {
        list->capacity *= 2;
        list->files = realloc(list->files, list->capacity * sizeof(char*));
    }
    list->files[list->count++] = strdup(path);
}

StringList* scan_directory(const char* dir_path, const char* extension) {
    StringList* list = malloc(sizeof(StringList));
    if (!list) return NULL;

    list->capacity = 32;
    list->count = 0;
    list->files = malloc(list->capacity * sizeof(char*));
    if (!list->files) {
        free(list);
        return NULL;
    }

    scan_directory_recursive(dir_path, extension, list);
    return list;
}

static void scan_directory_recursive(const char* dir_path, const char* extension, StringList* list) {
    DIR* dir = opendir(dir_path);
    if (!dir) {
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip hidden files and . and ..
        if (entry->d_name[0] == '.') continue;

        // Build full path
        char full_path[2048];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            // Recursively scan subdirectories
            scan_directory_recursive(full_path, extension, list);
        } else if (S_ISREG(st.st_mode)) {
            // Check extension
            const char* dot = strrchr(entry->d_name, '.');
            if (dot && strcmp(dot, extension) == 0) {
                string_list_add(list, full_path);
            }
        }
    }

    closedir(dir);
}

void string_list_free(StringList* list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) {
        free(list->files[i]);
    }
    free(list->files);
    free(list);
}

static const PluginConfig* plugin_config_from_so_name(const Config* cfg, const char* path) {
    if (!cfg || !path) return NULL;

    const char* filename = strrchr(path, '/');
    filename = filename ? filename + 1 : path;
    if (!filename[0]) return NULL;

    const char* dot = strrchr(filename, '.');
    size_t id_len = dot ? (size_t)(dot - filename) : strlen(filename);
    if (id_len == 0 || id_len >= 256) return NULL;

    char plugin_id[256];
    memcpy(plugin_id, filename, id_len);
    plugin_id[id_len] = '\0';

    for (size_t i = 0; i < cfg->plugins.count; i++) {
        PluginConfig* item = &cfg->plugins.items[i];
        if (item->plugin_id && strcmp(item->plugin_id, plugin_id) == 0) {
            return item;
        }
    }

    return NULL;
}

bool is_plugin_compatible(const char* path) {
    // Check if file exists and is readable
    if (access(path, R_OK) != 0) return false;

    // Try to open with dlopen to check compatibility
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        log_debug("[Plugin] File %s is not a loadable plugin: %s", path, dlerror());
        return false;
    }
    dlclose(handle);
    return true;
}

LoadedPlugin* load_plugin_from_file(PluginManager* manager, const char* path) {
    (void)manager;  // Reserved for future use (e.g., plugin dependency injection)

    LoadedPlugin* plugin = calloc(1, sizeof(LoadedPlugin));
    if (!plugin) return NULL;

    // Open the shared library
    plugin->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!plugin->handle) {
        log_error("[Plugin] Failed to load %s: %s", path, dlerror());
        free(plugin);
        return NULL;
    }

    // Get plugin_get_info function
    PluginGetInfoFunc get_info = (PluginGetInfoFunc)dlsym(plugin->handle, "plugin_get_info");
    if (!get_info) {
        log_error("[Plugin] No plugin_get_info in %s", path);
        dlclose(plugin->handle);
        free(plugin);
        return NULL;
    }

    // Get plugin info
    plugin->info = get_info();
    if (!plugin->info) {
        log_error("[Plugin] plugin_get_info returned NULL in %s", path);
        dlclose(plugin->handle);
        free(plugin);
        return NULL;
    }

    // Get optional init/cleanup functions
    plugin->init = (PluginInitFunc)dlsym(plugin->handle, "plugin_init");
    plugin->cleanup = (PluginCleanupFunc)dlsym(plugin->handle, "plugin_cleanup");

    // Set basic info
    plugin->path = strdup(path);
    plugin->name = strdup(plugin->info->name ? plugin->info->name : path);
    plugin->id = strdup(plugin->info->plugin_id ? plugin->info->plugin_id : plugin->name);
    plugin->type = plugin->info->type;

    log_debug("[Plugin] Loaded plugin: %s (%s)", plugin->name, plugin->id);
    return plugin;
}

// =============================================================================
// Plugin Manager Implementation
// =============================================================================

PluginManager* plugin_manager_new(const char* base_dir) {
    PluginManager* manager = calloc(1, sizeof(PluginManager));
    if (!manager) return NULL;

    pthread_mutex_init(&manager->lock, NULL);

    // Set up plugin directory for external plugins
    manager->external_dir = malloc(strlen(base_dir) + 16);
    sprintf(manager->external_dir, "%s/plugins", base_dir);

    // Create directory if it doesn't exist
    mkdir(manager->external_dir, 0755);

    log_debug("[PluginManager] Created at base: %s", base_dir);
    log_debug("[PluginManager] External dir: %s", manager->external_dir);

    return manager;
}

void plugin_manager_free(PluginManager* manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->lock);

    // Unload all plugins
    while (manager->plugins) {
        LoadedPlugin* plugin = manager->plugins;
        manager->plugins = plugin->next;
        plugin_manager_cleanup_plugin(plugin);
        free(plugin->path);
        free(plugin->name);
        free(plugin->id);
        free(plugin);
    }

    // Free command registrations
    for (size_t i = 0; i < manager->command_count; i++) {
        free(manager->commands[i].name);
        free(manager->commands[i].description);
    }
    free(manager->commands);

    pthread_mutex_unlock(&manager->lock);
    pthread_mutex_destroy(&manager->lock);

    free(manager->external_dir);
    free(manager);
}

int plugin_manager_load_external(PluginManager* manager) {
    log_debug("[PluginManager] Loading external plugins from %s", manager->external_dir);

    StringList* plugins = scan_directory(manager->external_dir, ".so");
    if (!plugins || plugins->count == 0) {
        log_debug("[PluginManager] No external plugins found");
        string_list_free(plugins);
        return 0;
    }

    int loaded = 0;
    for (size_t i = 0; i < plugins->count; i++) {
        const PluginConfig* plugin_cfg = plugin_config_from_so_name(manager->config, plugins->files[i]);
        if (!plugin_cfg || !plugin_cfg->enabled) {
            log_debug("[PluginManager] Plugin %s is disabled or not found in config", plugins->files[i]);
            continue;
        }
        if (plugin_manager_load_plugin(manager, plugins->files[i]) == 0) {
            loaded++;
        }
    }

    string_list_free(plugins);
    log_debug("[PluginManager] Loaded %d external plugins", loaded);
    return loaded;
}

int plugin_manager_load_plugin(PluginManager* manager, const char* path) {
    if (!manager || !path) return -1;

    pthread_mutex_lock(&manager->lock);

    // Check if already loaded
    LoadedPlugin* existing = NULL;
    for (LoadedPlugin* p = manager->plugins; p; p = p->next) {
        if (strcmp(p->path, path) == 0) {
            existing = p;
            break;
        }
    }

    if (existing) {
        log_debug("[PluginManager] Plugin already loaded: %s", path);
        pthread_mutex_unlock(&manager->lock);
        return 0;  // Already loaded
    }

    // Load the plugin
    LoadedPlugin* plugin = load_plugin_from_file(manager, path);
    if (!plugin) {
        pthread_mutex_unlock(&manager->lock);
        return -1;
    }

    // Unlock before calling plugin->init to avoid deadlock
    // (plugin init may call plugin_register_* which also needs the lock)
    pthread_mutex_unlock(&manager->lock);

    // Initialize the plugin (outside lock to avoid deadlock)
    int ret = 0;
    if (plugin->init) {
        ret = plugin->init(manager, plugin);
        if (ret != 0) {
            log_error("[PluginManager] Plugin init failed: %s", path);
            dlclose(plugin->handle);
            free(plugin->path);
            free(plugin->name);
            free(plugin->id);
            free(plugin);
            return -1;
        }
    }

    // Re-lock to add plugin to list
    pthread_mutex_lock(&manager->lock);

    // Add to list
    plugin->next = manager->plugins;
    manager->plugins = plugin;
    manager->plugin_count++;

    pthread_mutex_unlock(&manager->lock);
    log_info("[PluginManager] Loaded plugin: %s", plugin->name);
    return 0;
}

void plugin_manager_unload_plugin(PluginManager* manager, LoadedPlugin* plugin) {
    if (!manager || !plugin) return;

    // Call cleanup
    if (plugin->cleanup) {
        plugin->cleanup();
    }

    // Close the handle
    if (plugin->handle) {
        dlclose(plugin->handle);
    }
}

int plugin_manager_reload_plugin(PluginManager* manager, const char* plugin_id) {
    if (!manager || !plugin_id) return -1;

    PluginInitFunc init_fn = NULL;
    PluginCleanupFunc cleanup_fn = NULL;
    void* handle = NULL;
    const char* path = NULL;

    pthread_mutex_lock(&manager->lock);

    // Find the plugin
    LoadedPlugin* plugin = NULL;
    for (LoadedPlugin* p = manager->plugins; p; p = p->next) {
        if (strcmp(p->id, plugin_id) == 0 || strcmp(p->name, plugin_id) == 0) {
            plugin = p;
            break;
        }
    }

    if (!plugin) {
        log_error("[PluginManager] Plugin not found: %s", plugin_id);
        pthread_mutex_unlock(&manager->lock);
        return -1;
    }

    log_debug("[PluginManager] Reloading plugin: %s", plugin->name);

    // Call cleanup
    cleanup_fn = plugin->cleanup;
    if (cleanup_fn) {
        cleanup_fn();
    }

    // Close and reopen
    if (plugin->handle) {
        dlclose(plugin->handle);
        plugin->handle = NULL;
    }

    // Reload from file
    plugin->handle = dlopen(plugin->path, RTLD_NOW | RTLD_LOCAL);
    if (!plugin->handle) {
        log_error("[PluginManager] Failed to reload %s: %s", plugin->path, dlerror());
        pthread_mutex_unlock(&manager->lock);
        return -1;
    }

    // Re-get function pointers
    PluginGetInfoFunc get_info = (PluginGetInfoFunc)dlsym(plugin->handle, "plugin_get_info");
    plugin->init = (PluginInitFunc)dlsym(plugin->handle, "plugin_init");
    plugin->cleanup = (PluginCleanupFunc)dlsym(plugin->handle, "plugin_cleanup");

    if (get_info) {
        plugin->info = get_info();
    }

    // Re-initialize
    init_fn = plugin->init;
    handle = plugin->handle;
    path = plugin->path;

    pthread_mutex_unlock(&manager->lock);

    if (init_fn) {
        int ret = init_fn(manager, NULL);
        if (ret != 0) {
            log_error("[PluginManager] Plugin re-init failed: %s", path ? path : "(unknown)");
            pthread_mutex_lock(&manager->lock);
            for (LoadedPlugin* p = manager->plugins; p; p = p->next) {
                if (p == plugin && p->handle == handle) {
                    dlclose(p->handle);
                    p->handle = NULL;
                    break;
                }
            }
            pthread_mutex_unlock(&manager->lock);
            return -1;
        }
    }

    log_debug("[PluginManager] Plugin reloaded: %s", plugin->name);
    return 0;
}

LoadedPlugin* plugin_manager_find_plugin(PluginManager* manager, const char* plugin_id) {
    if (!manager || !plugin_id) return NULL;

    pthread_mutex_lock(&manager->lock);

    for (LoadedPlugin* p = manager->plugins; p; p = p->next) {
        if (strcmp(p->id, plugin_id) == 0) {
            LoadedPlugin* copy = calloc(1, sizeof(LoadedPlugin));
            if (!copy) {
                pthread_mutex_unlock(&manager->lock);
                return NULL;
            }
            copy->id = p->id ? strdup(p->id) : NULL;
            copy->name = p->name ? strdup(p->name) : NULL;
            copy->path = p->path ? strdup(p->path) : NULL;
            copy->type = p->type;
            copy->handle = NULL;
            copy->info = NULL;
            copy->init = NULL;
            copy->cleanup = NULL;
            copy->next = NULL;
            if ((p->id && !copy->id) || (p->name && !copy->name) || (p->path && !copy->path)) {
                free(copy->id);
                free(copy->name);
                free(copy->path);
                free(copy);
                pthread_mutex_unlock(&manager->lock);
                return NULL;
            }
            pthread_mutex_unlock(&manager->lock);
            return copy;
        }
    }

    pthread_mutex_unlock(&manager->lock);
    return NULL;
}

LoadedPlugin* plugin_manager_find_plugin_by_name(PluginManager* manager, const char* name) {
    if (!manager || !name) return NULL;

    pthread_mutex_lock(&manager->lock);

    for (LoadedPlugin* p = manager->plugins; p; p = p->next) {
        if (strcmp(p->name, name) == 0) {
            LoadedPlugin* copy = calloc(1, sizeof(LoadedPlugin));
            if (!copy) {
                pthread_mutex_unlock(&manager->lock);
                return NULL;
            }
            copy->id = p->id ? strdup(p->id) : NULL;
            copy->name = p->name ? strdup(p->name) : NULL;
            copy->path = p->path ? strdup(p->path) : NULL;
            copy->type = p->type;
            copy->handle = NULL;
            copy->info = NULL;
            copy->init = NULL;
            copy->cleanup = NULL;
            copy->next = NULL;
            if ((p->id && !copy->id) || (p->name && !copy->name) || (p->path && !copy->path)) {
                free(copy->id);
                free(copy->name);
                free(copy->path);
                free(copy);
                pthread_mutex_unlock(&manager->lock);
                return NULL;
            }
            pthread_mutex_unlock(&manager->lock);
            return copy;
        }
    }

    pthread_mutex_unlock(&manager->lock);
    return NULL;
}

void plugin_manager_free_plugin_snapshot(LoadedPlugin* plugin) {
    if (!plugin) return;
    free(plugin->id);
    free(plugin->name);
    free(plugin->path);
    free(plugin);
}

// =============================================================================
// Plugin Registration Functions
// =============================================================================

int plugin_register_tool(PluginManager* manager, LoadedPlugin* plugin,
                         const char* name, const char* desc,
                         const char* params, ToolExecuteFunc exec, void* user_data) {
    if (!manager) {
        log_error("[Plugin] PluginManager is NULL");
        return -1;
    }

    // Lock to protect access to manager->tool_registry
    pthread_mutex_lock(&manager->lock);

    if (!manager->tool_registry) {
        pthread_mutex_unlock(&manager->lock);
        log_error("[Plugin] Tool registry not available");
        return -1;
    }

    Error err = tool_registry_register_plugin_tool(manager->tool_registry, name, desc, params, exec, user_data, plugin);

    pthread_mutex_unlock(&manager->lock);

    if (err.code != ERR_NONE) {
        log_error("[Plugin] Failed to register tool %s: %s", name, err.message);
        return -1;
    }

    log_debug("[Plugin] Registered tool: %s from %s", name, plugin ? plugin->name : "builtin");
    return 0;
}

int plugin_register_channel(PluginManager* manager, LoadedPlugin* plugin,
                            const char* name, ChannelCreateFunc create) {
    // Channels are registered directly in main.c's channel array
    log_debug("[Plugin] Channel registration requested: %s from %s", name, plugin ? plugin->name : "builtin");

    if (!manager || !create) return -1;

    // Create the channel
    Channel* channel = create();
    if (!channel) {
        log_error("[Plugin] Failed to create channel: %s", name);
        return -1;
    }

    // Lock to protect access to manager->config, manager->bus, manager->channel_array
    pthread_mutex_lock(&manager->lock);

    // Initialize the channel with config and bus
    if (channel->init && manager->config && manager->bus) {
        pthread_mutex_unlock(&manager->lock);  // Unlock before init to avoid deadlock

        if (!channel->init(channel, manager->config, manager->bus)) {
            log_error("[Plugin] Channel %s init failed", name);
            channel->destroy(channel);
            return -1;
        }

        pthread_mutex_lock(&manager->lock);  // Re-lock for channel array access
    }

    // Add to manager's channel array
    if (manager->channel_array && manager->channel_count_ptr) {
        Channel** channels = (Channel**)manager->channel_array;
        int* count = manager->channel_count_ptr;
        if (manager->channel_capacity > 0 && *count >= manager->channel_capacity) {
            log_error("[Plugin] Failed to register channel %s: capacity reached (%d)", name, manager->channel_capacity);
            pthread_mutex_unlock(&manager->lock);
            channel->destroy(channel);
            return -1;
        }

        channels[*count] = channel;
        (*count)++;

        log_debug("[Plugin] Registered channel: %s (total: %d)", name, *count);
        pthread_mutex_unlock(&manager->lock);
        return 0;
    } else {
        log_error("[Plugin] Channel %s: channel_array=%p, channel_count_ptr=%p",
                  name, (void*)manager->channel_array, (void*)manager->channel_count_ptr);
        pthread_mutex_unlock(&manager->lock);
        channel->destroy(channel);
        return -1;
    }
}

int plugin_register_command(PluginManager* manager, LoadedPlugin* plugin,
                            const char* name, const char* desc, CommandFunc handler) {
    if (!manager || !name || !handler) return -1;

    pthread_mutex_lock(&manager->lock);

    // Check capacity
    if (manager->command_count >= manager->command_capacity) {
        size_t new_cap = manager->command_capacity ? manager->command_capacity * 2 : 16;
        CommandPluginDef* new_cmds = realloc(manager->commands, new_cap * sizeof(CommandPluginDef));
        if (!new_cmds) {
            pthread_mutex_unlock(&manager->lock);
            return -1;
        }
        manager->commands = new_cmds;
        manager->command_capacity = new_cap;
    }

    // Register the command
    manager->commands[manager->command_count].name = strdup(name);
    manager->commands[manager->command_count].description = strdup(desc);
    manager->commands[manager->command_count].handler = handler;
    manager->command_count++;

    log_debug("[Plugin] Registered command: /%s from %s", name, plugin ? plugin->name : "builtin");

    pthread_mutex_unlock(&manager->lock);
    return 0;
}

CommandPluginDef* plugin_manager_get_commands(PluginManager* manager, size_t* out_count) {
    if (!manager || !out_count) return NULL;

    pthread_mutex_lock(&manager->lock);
    *out_count = manager->command_count;
    pthread_mutex_unlock(&manager->lock);

    return manager->commands;
}
