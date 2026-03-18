/*
 * Echo Tool Plugin Demo
 *
 * A simple plugin that demonstrates the Primagen tool plugin API.
 * This tool echoes back the user's input with a prefix.
 *
 * Build: make
 * Install: cp echo_tool.so ../../.primagen/plugins/external/
 */

#include "../../../src/include/plugin.h"
#include "../../../src/plugin/plugin_manager.h"
#include "../../../src/include/common.h"
#include "../../../src/include/logger.h"
#include "../../../src/vendor/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =============================================================================
// Tool Implementation
// =============================================================================

static Error echo_tool_impl(void* user_data, const char* args_json, String* result) {
    (void)user_data;

    cJSON* json = cJSON_Parse(args_json);
    if (!json) {
        return error_new(ERR_JSON, "Invalid JSON arguments");
    }

    cJSON* message_item = cJSON_GetObjectItem(json, "message");
    if (!cJSON_IsString(message_item) || !message_item->valuestring) {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Missing 'message' argument");
    }

    const char* message = message_item->valuestring;

    // Echo back the message with a prefix
    char response[1024];
    snprintf(response, sizeof(response), "[Echo] You said: %s", message);

    *result = string_new(response);
    cJSON_Delete(json);

    log_info("[EchoTool] Echoed message: %s", message);
    return error_new(ERR_NONE, "");
}

// =============================================================================
// Plugin Initialization
// =============================================================================

PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context) {
    (void)context;

    log_info("[Plugin:echo_tool] Initializing echo tool plugin");

    // Register the echo tool
    int ret = plugin_register_tool(manager, NULL, "echo_tool",
        "Echo back the user's message with a prefix. Useful for testing plugin system.",
        "{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\",\"description\":\"The message to echo\"}},\"required\":[\"message\"]}",
        echo_tool_impl, NULL);

    if (ret == 0) {
        log_info("[Plugin:echo_tool] Successfully registered echo_tool");
    } else {
        log_error("[Plugin:echo_tool] Failed to register echo_tool");
        return -1;
    }

    return 0;
}

PLUGIN_EXPORT int plugin_cleanup(void) {
    log_info("[Plugin:echo_tool] Cleaning up echo tool plugin");
    return 0;
}

// =============================================================================
// Plugin Information
// =============================================================================

static PluginInfo g_plugin_info = {
    .version = 1,
    .type = PLUGIN_TOOL,
    .name = "echo_tool",
    .description = "A demo plugin that echoes user messages",
    .plugin_id = "echo_tool_demo"
};

PLUGIN_EXPORT PluginInfo* plugin_get_info(void) {
    return &g_plugin_info;
}
