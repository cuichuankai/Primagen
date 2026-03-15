/*
 * MCP Tools Integration
 * Bridges MCP tools to Primagen's ToolRegistry
 */

#include "mcp.h"
#include "../tools/tool.h"
#include "../include/logger.h"
#include "../include/common.h"
#include <string.h>

// MCP tool context
typedef struct {
    MCPClient* client;
    char* tool_name;
} MCPToolContext;

// Execute MCP tool
static Error mcp_tool_execute(void* user_data, const char* params, String* output) {
    MCPToolContext* ctx = (MCPToolContext*)user_data;

    if (!ctx || !ctx->client || !ctx->tool_name) {
        return error_new(ERR_INVALID_PARAM, "Invalid MCP tool context");
    }

    log_info("[MCP Tool] Calling %s with params: %s", ctx->tool_name, params);

    // Call MCP tool
    String result = { NULL, 0 };
    Error err = mcp_client_call_tool(ctx->client, ctx->tool_name, params, &result);

    if (err.code != ERR_NONE) {
        log_error("[MCP Tool] Call failed: %s", err.message);
        string_append(output, err.message);
        return err;
    }

    // Copy result to output
    if (result.data) {
        string_append(output, result.data);
        free(result.data);
    } else {
        string_append(output, "No result from MCP tool");
    }

    log_debug("[MCP Tool] Call completed");
    return error_new(ERR_NONE, "");
}

// Free MCP tool context
static void mcp_tool_free(void* user_data) {
    MCPToolContext* ctx = (MCPToolContext*)user_data;
    if (ctx) {
        free(ctx->tool_name);
        free(ctx);
    }
}

// Register all MCP tools from a client to ToolRegistry
void mcp_register_tools(ToolRegistry* reg, MCPClient* client) {
    if (!reg || !client || !client->tools || client->tools_count == 0) {
        log_info("[MCP] No tools to register");
        return;
    }

    log_info("[MCP] Registering %zu tools from %s", client->tools_count, client->server_id);

    for (size_t i = 0; i < client->tools_count; i++) {
        MCPToolDef* tool = &client->tools[i];

        // Create tool context
        MCPToolContext* ctx = malloc(sizeof(MCPToolContext));
        if (!ctx) {
            log_error("[MCP] Failed to allocate context for tool: %s", tool->name);
            continue;
        }
        ctx->client = client;
        ctx->tool_name = strdup(tool->name);

        // Build tool name with prefix to avoid conflicts
        char full_name[256];
        snprintf(full_name, sizeof(full_name), "amap_%s", tool->name);

        // Register tool
        Error err = tool_registry_register(reg, full_name, tool->description,
                                           tool->input_schema.data,
                                           mcp_tool_execute, ctx);

        if (err.code != ERR_NONE) {
            log_error("[MCP] Failed to register tool %s: %s", full_name, err.message);
            mcp_tool_free(ctx);
        } else {
            log_info("[MCP] Registered tool: %s", full_name);
        }
    }
}
