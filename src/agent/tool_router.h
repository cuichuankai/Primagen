#ifndef TOOL_ROUTER_H
#define TOOL_ROUTER_H

#include <stddef.h>
#include <stdbool.h>
#include "../include/message.h"
#include "../tools/tool.h"
#include "../session/session.h"

struct AgentLoop;

typedef struct {
    struct AgentLoop* loop;
    ToolRegistry* tool_registry;
} ToolRouter;

ToolRouter* tool_router_new(struct AgentLoop* loop, ToolRegistry* tool_registry);
void tool_router_free(ToolRouter* router);

Error tool_router_execute(ToolRouter* router, const char* tool_name, const char* args_json, String* result);

bool tool_router_handle_fallback_command(ToolRouter* router, InboundMessage* inbound, const char* full_content);

Tool* tool_router_get_tool(ToolRouter* router, const char* tool_name);

#endif // TOOL_ROUTER_H
