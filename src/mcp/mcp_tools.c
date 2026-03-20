/*
 * MCP Tools Integration
 * Bridges MCP tools to Primagen's ToolRegistry
 */

#include "mcp.h"
#include "../tools/tool.h"
#include "../include/logger.h"
#include "../include/common.h"
#include "../vendor/cJSON/cJSON.h"
#include <string.h>

// MCP tool context
typedef struct {
    MCPClient* client;
    char* tool_name;
} MCPToolContext;

// MCP resource context
typedef struct {
    MCPClient* client;
} MCPResourceContext;

// MCP prompt context
typedef struct {
    MCPClient* client;
} MCPPromptContext;

static bool mcp_error_contains(const char* message, const char* needle) {
    return message && needle && strstr(message, needle) != NULL;
}

static bool mcp_needs_reinitialize(const Error* err) {
    if (!err) return false;
    return (mcp_error_contains(err->message, "without mcp-session-id header") &&
            mcp_error_contains(err->message, "initialize request")) ||
           (mcp_error_contains(err->message, "session") &&
            mcp_error_contains(err->message, "expired"));
}

static bool mcp_can_retry(const Error* err) {
    if (!err) return false;
    if (mcp_needs_reinitialize(err)) return true;
    return err->code == ERR_TIMEOUT || err->code == ERR_CONNECTION;
}

// Execute MCP tool
static Error mcp_tool_execute(void* user_data, const char* params, String* output) {
    MCPToolContext* ctx = (MCPToolContext*)user_data;

    if (!ctx || !ctx->client || !ctx->tool_name) {
        return error_new(ERR_INVALID_PARAM, "Invalid MCP tool context");
    }

    log_debug("[MCP Tool] Calling %s with params: %s", ctx->tool_name, params);

    // Call MCP tool
    String result = { NULL, 0 };
    Error err = mcp_client_call_tool(ctx->client, ctx->tool_name, params, &result);

    if (err.code != ERR_NONE && mcp_can_retry(&err)) {
        if (mcp_needs_reinitialize(&err)) {
            log_warn("[MCP Tool] Session missing, re-initializing MCP client before retry");
            ctx->client->initialized = false;
            MCPToolDef* refreshed_tools = NULL;
            size_t refreshed_count = 0;
            Error reinit_err = mcp_client_list_tools(ctx->client, &refreshed_tools, &refreshed_count);
            if (reinit_err.code != ERR_NONE) {
                log_error("[MCP Tool] MCP re-initialize failed: %s", reinit_err.message);
            }
        }
        log_warn("[MCP Tool] Call failed, retry once for %s: %s", ctx->tool_name, err.message);
        err = mcp_client_call_tool(ctx->client, ctx->tool_name, params, &result);
    }

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

// List MCP resources
static Error mcp_list_resources_execute(void* user_data, const char* params, String* output) {
    (void)params;  // Not used - no parameters needed
    MCPResourceContext* ctx = (MCPResourceContext*)user_data;

    if (!ctx || !ctx->client) {
        return error_new(ERR_INVALID_PARAM, "Invalid MCP resource context");
    }

    log_debug("[MCP] Listing resources from %s", ctx->client->server_id);

    MCPResource* resources = NULL;
    size_t count = 0;
    Error err = mcp_client_list_resources(ctx->client, &resources, &count);

    if (err.code != ERR_NONE) {
        log_error("[MCP] List resources failed: %s", err.message);
        string_append(output, err.message);
        return err;
    }

    // Build JSON response
    string_append(output, "[");
    for (size_t i = 0; i < count; i++) {
        if (i > 0) string_append(output, ",");
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "{\"uri\":\"%s\",\"name\":\"%s\",\"description\":\"%s\",\"mimeType\":\"%s\"}",
            resources[i].uri ? resources[i].uri : "",
            resources[i].name ? resources[i].name : "",
            resources[i].description ? resources[i].description : "",
            resources[i].mime_type ? resources[i].mime_type : "");
        string_append(output, buf);
    }
    string_append(output, "]");

    log_debug("[MCP] Listed %zu resources", count);
    return error_new(ERR_NONE, "");
}

// Read MCP resource
static Error mcp_read_resource_execute(void* user_data, const char* params, String* output) {
    MCPResourceContext* ctx = (MCPResourceContext*)user_data;

    if (!ctx || !ctx->client) {
        return error_new(ERR_INVALID_PARAM, "Invalid MCP resource context");
    }

    // Parse URI from params
    cJSON* root = cJSON_Parse(params);
    if (!root) {
        return error_new(ERR_JSON, "Invalid JSON params");
    }

    cJSON* uri_json = cJSON_GetObjectItem(root, "uri");
    if (!uri_json || !uri_json->valuestring) {
        cJSON_Delete(root);
        return error_new(ERR_INVALID_PARAM, "Missing 'uri' parameter");
    }

    const char* uri = uri_json->valuestring;
    cJSON_Delete(root);

    log_debug("[MCP] Reading resource %s from %s", uri, ctx->client->server_id);

    String content = { NULL, 0 };
    Error err = mcp_client_read_resource(ctx->client, uri, &content);

    if (err.code != ERR_NONE) {
        log_error("[MCP] Read resource failed: %s", err.message);
        string_append(output, err.message);
        return err;
    }

    // Return content
    if (content.data) {
        string_append(output, content.data);
        free(content.data);
    } else {
        string_append(output, "No content");
    }

    log_debug("[MCP] Read resource completed");
    return error_new(ERR_NONE, "");
}

// List MCP prompts
static Error mcp_list_prompts_execute(void* user_data, const char* params, String* output) {
    (void)params;  // Not used - no parameters needed
    MCPPromptContext* ctx = (MCPPromptContext*)user_data;

    if (!ctx || !ctx->client) {
        return error_new(ERR_INVALID_PARAM, "Invalid MCP prompt context");
    }

    log_debug("[MCP] Listing prompts from %s", ctx->client->server_id);

    MCPPrompt* prompts = NULL;
    size_t count = 0;
    Error err = mcp_client_list_prompts(ctx->client, &prompts, &count);

    if (err.code != ERR_NONE) {
        log_error("[MCP] List prompts failed: %s", err.message);
        string_append(output, err.message);
        return err;
    }

    // Build JSON response
    string_append(output, "[");
    for (size_t i = 0; i < count; i++) {
        if (i > 0) string_append(output, ",");
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "{\"name\":\"%s\",\"description\":\"%s\"}",
            prompts[i].name ? prompts[i].name : "",
            prompts[i].description ? prompts[i].description : "");
        string_append(output, buf);
    }
    string_append(output, "]");

    log_debug("[MCP] Listed %zu prompts", count);
    return error_new(ERR_NONE, "");
}

// Free MCP resource context
static void mcp_resource_free(void* user_data) {
    MCPResourceContext* ctx = (MCPResourceContext*)user_data;
    free(ctx);
}

// Free MCP prompt context
static void mcp_prompt_free(void* user_data) {
    MCPPromptContext* ctx = (MCPPromptContext*)user_data;
    free(ctx);
}

// Register all MCP tools from a client to ToolRegistry
void mcp_register_tools(ToolRegistry* reg, MCPClient* client) {
    if (!reg || !client || !client->tools || client->tools_count == 0) {
        log_debug("[MCP] No tools to register");
        return;
    }

    log_debug("[MCP] Registering %zu tools from %s", client->tools_count, client->server_id);

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
        snprintf(full_name, sizeof(full_name), "mcp_%s", tool->name);

        // Register tool
        Error err = tool_registry_register(reg, full_name, tool->description,
                                           tool->input_schema.data,
                                           mcp_tool_execute, ctx);

        if (err.code != ERR_NONE) {
            log_error("[MCP] Failed to register tool %s: %s", full_name, err.message);
            mcp_tool_free(ctx);
        } else {
            log_debug("[MCP] Registered tool: %s", full_name);
        }
    }
}

// Register MCP resources and prompts tools
void mcp_register_resources_prompts(ToolRegistry* reg, MCPClient* client) {
    if (!reg || !client) {
        log_debug("[MCP] Invalid arguments for registering resources/prompts");
        return;
    }

    log_debug("[MCP] Registering resources and prompts tools for %s", client->server_id);

    // Create resource context
    MCPResourceContext* res_ctx = malloc(sizeof(MCPResourceContext));
    if (!res_ctx) {
        log_error("[MCP] Failed to allocate resource context");
        return;
    }
    res_ctx->client = client;

    // Register mcp_list_resources
    Error err = tool_registry_register(reg, "mcp_list_resources",
        "List available resources from MCP servers",
        "{}",
        mcp_list_resources_execute, res_ctx);

    if (err.code != ERR_NONE) {
        log_error("[MCP] Failed to register mcp_list_resources: %s", err.message);
        mcp_resource_free(res_ctx);
    } else {
        log_debug("[MCP] Registered tool: mcp_list_resources");
    }

    // Register mcp_read_resource
    err = tool_registry_register(reg, "mcp_read_resource",
        "Read content from an MCP resource by URI",
        "{\"type\":\"object\",\"properties\":{\"uri\":{\"type\":\"string\",\"description\":\"Resource URI\"}},\"required\":[\"uri\"]}",
        mcp_read_resource_execute, res_ctx);

    if (err.code != ERR_NONE) {
        log_error("[MCP] Failed to register mcp_read_resource: %s", err.message);
    } else {
        log_debug("[MCP] Registered tool: mcp_read_resource");
    }

    // Create prompt context
    MCPPromptContext* prompt_ctx = malloc(sizeof(MCPPromptContext));
    if (!prompt_ctx) {
        log_error("[MCP] Failed to allocate prompt context");
        return;
    }
    prompt_ctx->client = client;

    // Register mcp_list_prompts
    err = tool_registry_register(reg, "mcp_list_prompts",
        "List available prompts from MCP servers",
        "{}",
        mcp_list_prompts_execute, prompt_ctx);

    if (err.code != ERR_NONE) {
        log_error("[MCP] Failed to register mcp_list_prompts: %s", err.message);
        mcp_prompt_free(prompt_ctx);
    } else {
        log_debug("[MCP] Registered tool: mcp_list_prompts");
    }
}
