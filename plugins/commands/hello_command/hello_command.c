/*
 * Hello Command Plugin Demo
 *
 * A simple plugin that demonstrates the Primagen command plugin API.
 * This plugin adds a /hello slash command.
 *
 * Build: make
 * Install: cp hello_command.so ../../.primagen/plugins/external/
 */

#include "../../../src/include/plugin.h"
#include "../../../src/plugin/plugin_manager.h"
#include "../../../src/include/common.h"
#include "../../../src/include/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =============================================================================
// Command Handler
// =============================================================================

static int cmd_hello(CommandContext* ctx, Config* cfg, const char* workspace_path, int argc, char** argv) {
    (void)cfg;
    (void)argc;
    (void)argv;
    if (!ctx || !ctx->send_response) {
        log_error("[HelloCommand] Command context is invalid");
        return -1;
    }

    char response[512];
    snprintf(response, sizeof(response),
        "Hello from Primagen plugin system!\n"
        "  Workspace: %s\n"
        "  This is a demo command plugin using CommandContext.",
        workspace_path ? workspace_path : "(none)");

    int ret = ctx->send_response(ctx, response);
    log_info("[HelloCommand] Executed /hello command");
    return ret;
}

// =============================================================================
// Plugin Initialization
// =============================================================================

PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context) {
    (void)context;

    log_info("[Plugin:hello_command] Initializing hello command plugin");

    // Register the /hello command
    int ret = plugin_register_command(manager, NULL, "hello",
        "Print a hello message from the plugin system",
        cmd_hello);

    if (ret == 0) {
        log_info("[Plugin:hello_command] Successfully registered /hello command");
    } else {
        log_error("[Plugin:hello_command] Failed to register /hello command");
        return -1;
    }

    return 0;
}

PLUGIN_EXPORT int plugin_cleanup(void) {
    log_info("[Plugin:hello_command] Cleaning up hello command plugin");
    return 0;
}

// =============================================================================
// Plugin Information
// =============================================================================

static PluginInfo g_plugin_info = {
    .version = 1,
    .type = PLUGIN_COMMAND,
    .name = "hello_command",
    .description = "A demo command plugin that adds /hello slash command",
    .plugin_id = "hello_command_demo"
};

PLUGIN_EXPORT PluginInfo* plugin_get_info(void) {
    return &g_plugin_info;
}
