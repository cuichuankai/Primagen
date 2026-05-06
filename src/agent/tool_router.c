#include "tool_router.h"
#include "agent_common.h"
#include "agent_loop.h"
#include "../include/logger.h"
#include "../include/utils.h"
#include "../tools/tool_executor.h"
#include "../tools/tools_impl.h"
#include "../vendor/cJSON/cJSON.h"
#include <string.h>
#include <stdio.h>


ToolRouter* tool_router_new(struct AgentLoop* loop, ToolRegistry* tool_registry) {
    if (!loop || !tool_registry) return NULL;
    
    ToolRouter* router = calloc(1, sizeof(ToolRouter));
    if (!router) return NULL;
    
    router->loop = loop;
    router->tool_registry = tool_registry;
    return router;
}

void tool_router_free(ToolRouter* router) {
    if (router) {
        free(router);
    }
}

Error tool_router_execute(ToolRouter* router, const char* tool_name, const char* args_json, String* result) {
    if (!router || !tool_name || !args_json || !result) {
        return error_new(ERR_INVALID_PARAM, "Invalid parameters");
    }
    
    Tool* tool = tool_router_get_tool(router, tool_name);
    if (!tool) {
        return error_new(ERR_TOOL, "Tool not found");
    }
    
    log_debug("[ToolRouter] Executing tool: %s", tool_name);
    
    Error err = tool_registry_execute(router->tool_registry, tool_name, args_json, result);
    
    if (err.code != ERR_NONE) {
        log_error("[ToolRouter] Tool execution failed: %s", err.message);
    } else {
        log_debug("[ToolRouter] Tool execution successful: %s", tool_name);
    }
    
    return err;
}

bool tool_router_handle_fallback_command(ToolRouter* router, InboundMessage* inbound, const char* full_content) {
    if (!router || !inbound || !full_content) return false;
    
    char tool_name[64] = {0};
    const char* args_start = NULL;
    parse_slash_command(full_content, tool_name, sizeof(tool_name), &args_start);
    
    for (size_t i = 0; i < router->tool_registry->count; i++) {
        Tool* tool = &router->tool_registry->tools[i];
        if (tool && strcmp(tool->def.name.data, tool_name) == 0) {
            log_debug("[ToolRouter] Tool fallback: /%s -> %s", tool_name, tool->def.name.data);
            
            cJSON* args_obj = cJSON_CreateObject();
            if (args_start) {
                cJSON_AddStringToObject(args_obj, "args", args_start);
            } else {
                cJSON_AddStringToObject(args_obj, "args", "");
            }
            char* args_json_str = cJSON_PrintUnformatted(args_obj);
            cJSON_Delete(args_obj);
            String args_json = string_new(args_json_str ? args_json_str : "{\"args\":\"\"}");
            free(args_json_str);
            
            String result = string_new("");
            Error err = tool_router_execute(router, tool_name, args_json.data, &result);
            
            if (err.code == ERR_NONE && result.data) {
                OutboundMessage* outbound = outbound_message_new(
                    inbound->channel.data, 
                    inbound->chat_id.data, 
                    result.data
                );
                if (outbound) {
                    message_bus_send_outbound(router->loop->bus, outbound);
                }
            } else {
                char response[256];
                snprintf(response, sizeof(response), "Error executing tool: %s", err.message);
                OutboundMessage* outbound = outbound_message_new(
                    inbound->channel.data, 
                    inbound->chat_id.data, 
                    response
                );
                if (outbound) {
                    message_bus_send_outbound(router->loop->bus, outbound);
                }
            }
            
            string_free(&args_json);
            string_free(&result);
            
            return true;
        }
    }
    
    return false;
}

Tool* tool_router_get_tool(ToolRouter* router, const char* tool_name) {
    if (!router || !tool_name) return NULL;
    
    for (size_t i = 0; i < router->tool_registry->count; i++) {
        Tool* tool = &router->tool_registry->tools[i];
        if (tool && strcmp(tool->def.name.data, tool_name) == 0) {
            return tool;
        }
    }
    
    return NULL;
}
