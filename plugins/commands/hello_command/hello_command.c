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
#include "../../../src/include/config.h"
#include "../../../src/bus/message_bus.h"
#include "../../../src/include/message.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =============================================================================
// Command Handler
// =============================================================================

// Standard argv layout for all commands:
// argv[0] = AgentLoop*
// argv[1] = session_key
// argv[2] = channel
// argv[3] = chat_id
// argv[4] = MessageBus*
// argv[5+] = user arguments

static int cmd_hello(Config* cfg, const char* workspace_path, int argc, char** argv) {
    (void)cfg;

    // Check minimum argument count (5 base parameters + 0 user args)
    if (argc < 5) {
        log_error("[HelloCommand] Invalid argument count: %d", argc);
        return -1;
    }

    // Extract standard parameters using direct indexing
    // Note: In production code, consider using macros from agent_loop.h
    void* loop = argv[0];  // AgentLoop* (opaque pointer for plugins)
    const char* channel = argv[2];
    const char* chat_id = argv[3];
    MessageBus* bus = (MessageBus*)argv[4];

    (void)loop;  // Could be used for accessing AgentLoop internals if needed

    // Build response message
    char response[512];
    snprintf(response, sizeof(response),
        "Hello from Primagen plugin system!\n"
        "  Workspace: %s\n"
        "  This is a demo command plugin showing proper parameter access.",
        workspace_path ? workspace_path : "(none)");

    // Send response through message bus
    OutboundMessage* outbound = outbound_message_new(channel, chat_id, response);
    message_bus_send_outbound(bus, outbound);

    log_info("[HelloCommand] Executed /hello command for channel=%s, chat_id=%s", channel, chat_id);
    return 0;
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
