#include "../../../src/include/plugin.h"
#include "../../../src/plugin/plugin_manager.h"
#include "../../../src/include/config.h"
#include "../../../src/include/logger.h"
#include "mcp.h"

typedef struct {
    MCPManager* manager;
} MCPPluginRuntime;

static MCPPluginRuntime g_runtime = {0};

PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context) {
    if (!manager || !manager->config) return -1;

    LoadedPlugin* plugin = (LoadedPlugin*)context;
    PluginConfig* cfg = config_get_plugin_config(manager->config, "mcp_tools");
    if (!cfg || !cfg->config) {
        log_error("[Plugin:mcp_tools] Missing plugin config");
        return -1;
    }

    g_runtime.manager = mcp_manager_create(manager->external_dir ? manager->external_dir : ".");
    if (!g_runtime.manager) {
        log_error("[Plugin:mcp_tools] Failed to create MCP manager");
        return -1;
    }

    Error err = mcp_manager_setup_from_plugin_config(g_runtime.manager, cfg->config, manager, plugin);
    if (err.code != ERR_NONE) {
        log_error("[Plugin:mcp_tools] Setup failed: %s", err.message);
        mcp_manager_free(g_runtime.manager);
        g_runtime.manager = NULL;
        return -1;
    }

    log_info("[Plugin:mcp_tools] Loaded %zu MCP clients", g_runtime.manager->clients_count);
    return 0;
}

PLUGIN_EXPORT int plugin_cleanup(void) {
    if (g_runtime.manager) {
        mcp_manager_free(g_runtime.manager);
        g_runtime.manager = NULL;
    }
    return 0;
}

static PluginInfo g_plugin_info = {
    .version = 1,
    .type = PLUGIN_TOOL,
    .name = "mcp_tools",
    .description = "MCP tool bridge plugin",
    .plugin_id = "mcp_tools"
};

PLUGIN_EXPORT PluginInfo* plugin_get_info(void) {
    return &g_plugin_info;
}
