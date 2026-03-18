#include "acp.h"
#include "../include/logger.h"
#include "../include/config.h"
#include "../vendor/cJSON/cJSON.h"
#include "../vendor/mongoose/mongoose.h"
#include <pthread.h>
#include <time.h>

// Forward declarations
static void send_json_response(struct mg_connection* nc, int status, const char* json);
static void send_error_response(struct mg_connection* nc, int code, const char* message);
static char* generate_response_id(void);

// Generate unique response ID
static char* generate_response_id(void) {
    static pthread_mutex_t id_mutex = PTHREAD_MUTEX_INITIALIZER;
    static unsigned long counter = 0;

    pthread_mutex_lock(&id_mutex);
    char* id = malloc(64);
    if (id) {
        snprintf(id, 64, "chatcmpl-%lu-%ld", counter++, time(NULL));
    }
    pthread_mutex_unlock(&id_mutex);
    return id;
}

// Send JSON response
static void send_json_response(struct mg_connection* nc, int status, const char* json) {
    mg_http_reply(nc, status, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n", "%s", json);
}

// Send error response
static void send_error_response(struct mg_connection* nc, int code, const char* message) {
    char* error_json = acp_error_json(code, message);
    int status = 400;
    if (code == 401) status = 401;
    else if (code == 404) status = 404;
    else if (code == 500) status = 500;
    send_json_response(nc, status, error_json);
    free(error_json);
}

// Create ACPServer instance
ACPServer* acp_server_new(
    MessageBus* bus,
    ToolRegistry* tool_registry,
    AgentLoop* agent_loop,
    SessionManager* session_mgr,
    Config* config
) {
    ACPServer* server = calloc(1, sizeof(ACPServer));
    if (!server) return NULL;

    server->bus = bus;
    server->tool_registry = tool_registry;
    server->agent_loop = agent_loop;
    server->session_mgr = session_mgr;
    server->config = config;
    server->running = false;
    server->mgr = NULL;
    server->nc = NULL;
    server->active_connections = 0;
    server->host = strdup(ACP_DEFAULT_HOST);
    server->port = ACP_DEFAULT_PORT;

    pthread_mutex_init(&server->connection_mutex, NULL);

    log_debug("[ACP] Server created");
    return server;
}

// Free ACPServer
void acp_server_free(ACPServer* server) {
    if (!server) return;

    pthread_mutex_destroy(&server->connection_mutex);
    free(server->host);
    free(server);
    log_debug("[ACP] Server freed");
}

// Start ACP server
int acp_server_start(ACPServer* server, int port, const char* host) {
    if (!server || server->running) return -1;

    server->port = port;
    if (host) {
        free(server->host);
        server->host = strdup(host);
    }

    char listen_addr[64];
    snprintf(listen_addr, sizeof(listen_addr), "http://%s:%d", server->host, server->port);

    server->mgr = calloc(1, sizeof(struct mg_mgr));
    if (!server->mgr) {
        log_error("[ACP] Failed to allocate mongoose manager");
        return -1;
    }

    mg_mgr_init(server->mgr);

    server->nc = mg_http_listen(server->mgr, listen_addr, acp_event_handler, server);
    if (!server->nc) {
        log_error("[ACP] Failed to listen on %s", listen_addr);
        free(server->mgr);
        server->mgr = NULL;
        return -1;
    }

    server->running = true;
    log_debug("[ACP] Server started on %s", listen_addr);

    return 0;
}

// Stop ACP server
void acp_server_stop(ACPServer* server) {
    if (!server || !server->running) return;

    server->running = false;

    if (server->nc) {
        mg_close_conn(server->nc);
        server->nc = NULL;
    }

    if (server->mgr) {
        mg_mgr_free(server->mgr);
        free(server->mgr);
        server->mgr = NULL;
    }

    log_debug("[ACP] Server stopped");
}

// HTTP event handler
void acp_event_handler(struct mg_connection* nc, int ev, void* ev_data) {
    if (ev != MG_EV_HTTP_MSG) return;

    ACPServer* server = (ACPServer*)nc->fn_data;
    struct mg_http_message* hm = (struct mg_http_message*)ev_data;

    // Track connections
    pthread_mutex_lock(&server->connection_mutex);
    server->active_connections++;
    pthread_mutex_unlock(&server->connection_mutex);

    log_debug("[ACP] %.*s %.*s", (int)hm->method.len, hm->method.buf, (int)hm->uri.len, hm->uri.buf);

    // Route requests
    if (mg_match(hm->uri, mg_str(ACP_ROUTE_HEALTH), NULL)) {
        acp_handle_health(nc, server);
    }
    else if (mg_match(hm->uri, mg_str(ACP_ROUTE_TOOLS_LIST), NULL)) {
        acp_handle_tools_list(nc, server);
    }
    else if (mg_match(hm->uri, mg_str(ACP_ROUTE_TOOLS_CALL), NULL)) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            send_error_response(nc, 405, "Method not allowed");
        } else {
            // Extract body as null-terminated string
            char* body = malloc(hm->body.len + 1);
            if (body) {
                memcpy(body, hm->body.buf, hm->body.len);
                body[hm->body.len] = '\0';
                acp_handle_tools_call(nc, server, body);
                free(body);
            } else {
                send_error_response(nc, 500, "Failed to allocate memory for request body");
            }
        }
    }
    else if (mg_match(hm->uri, mg_str(ACP_ROUTE_CHAT_COMPLETIONS), NULL)) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            send_error_response(nc, 405, "Method not allowed");
        } else {
            // Extract body as null-terminated string
            char* body = malloc(hm->body.len + 1);
            if (body) {
                memcpy(body, hm->body.buf, hm->body.len);
                body[hm->body.len] = '\0';
                acp_handle_chat_completions(nc, server, body);
                free(body);
            } else {
                send_error_response(nc, 500, "Failed to allocate memory for request body");
            }
        }
    }
    else {
        send_error_response(nc, 404, "Not found");
    }

    // Track connection release
    pthread_mutex_lock(&server->connection_mutex);
    server->active_connections--;
    pthread_mutex_unlock(&server->connection_mutex);
}

// Handle GET /v1/health
void acp_handle_health(struct mg_connection* nc, ACPServer* server) {
    (void)server;
    const char* health_json = "{\"status\":\"ok\",\"running\":true}";
    send_json_response(nc, 200, health_json);
}

// Handle GET /v1/tools/list
void acp_handle_tools_list(struct mg_connection* nc, ACPServer* server) {
    if (!server || !server->tool_registry) {
        send_error_response(nc, 500, "Tool registry not initialized");
        return;
    }

    char* json = acp_tools_list_json(server->tool_registry);
    if (json) {
        send_json_response(nc, 200, json);
        free(json);
    } else {
        send_error_response(nc, 500, "Failed to serialize tools");
    }
}

// Handle POST /v1/tools/call
void acp_handle_tools_call(struct mg_connection* nc, ACPServer* server, const char* body) {
    if (!server || !server->tool_registry) {
        send_error_response(nc, 500, "Tool registry not initialized");
        return;
    }

    // Parse request JSON
    cJSON* root = cJSON_Parse(body);
    if (!root) {
        send_error_response(nc, 400, "Invalid JSON");
        return;
    }

    cJSON* tool_name_json = cJSON_GetObjectItem(root, "tool_name");
    cJSON* arguments_json = cJSON_GetObjectItem(root, "arguments");
    // Note: session_id extracted but not used in current implementation
    cJSON_GetObjectItem(root, "session_id");

    if (!tool_name_json || tool_name_json->type != cJSON_String) {
        cJSON_Delete(root);
        send_error_response(nc, 400, "Missing or invalid 'tool_name'");
        return;
    }

    const char* tool_name = tool_name_json->valuestring;

    // Get arguments as JSON string
    char* arguments_str = NULL;
    if (arguments_json) {
        arguments_str = cJSON_PrintUnformatted(arguments_json);
    } else {
        arguments_str = strdup("{}");
    }

    log_debug("[ACP] Tool call request: %s", tool_name);

    // Execute tool
    String result = string_new("");
    Error err = tool_registry_execute(server->tool_registry, tool_name, arguments_str, &result);

    char* response_json;
    if (err.code != ERR_NONE) {
        log_error("[ACP] Tool execution failed: %s", err.message);
        response_json = acp_tool_response_json(tool_name, NULL, err.message);
    } else {
        log_debug("[ACP] Tool execution success: %s", result.data);
        response_json = acp_tool_response_json(tool_name, result.data, NULL);
    }

    send_json_response(nc, 200, response_json);

    // Cleanup
    free(response_json);
    free(arguments_str);
    string_free(&result);
    cJSON_Delete(root);
}

// Handle POST /v1/chat/completions
void acp_handle_chat_completions(struct mg_connection* nc, ACPServer* server, const char* body) {
    if (!server || !server->agent_loop) {
        send_error_response(nc, 500, "Agent loop not initialized");
        return;
    }

    // Parse request JSON
    cJSON* root = cJSON_Parse(body);
    if (!root) {
        send_error_response(nc, 400, "Invalid JSON");
        return;
    }

    // Extract fields
    cJSON* messages_json = cJSON_GetObjectItem(root, "messages");
    cJSON* model_json = cJSON_GetObjectItem(root, "model");
    // Note: temperature, max_tokens, stream extracted but not used in current implementation
    cJSON_GetObjectItem(root, "temperature");
    cJSON_GetObjectItem(root, "max_tokens");
    cJSON_GetObjectItem(root, "stream");
    cJSON* session_id_json = cJSON_GetObjectItem(root, "session_id");

    if (!messages_json || messages_json->type != cJSON_Array) {
        cJSON_Delete(root);
        send_error_response(nc, 400, "Missing or invalid 'messages' array");
        return;
    }

    // Get last user message
    const char* user_message = NULL;
    int msg_count = cJSON_GetArraySize(messages_json);
    for (int i = msg_count - 1; i >= 0; i--) {
        cJSON* msg = cJSON_GetArrayItem(messages_json, i);
        cJSON* role = cJSON_GetObjectItem(msg, "role");
        cJSON* content = cJSON_GetObjectItem(msg, "content");
        if (role && content && role->valuestring && strcmp(role->valuestring, "user") == 0) {
            user_message = content->valuestring;
            break;
        }
    }

    if (!user_message) {
        cJSON_Delete(root);
        send_error_response(nc, 400, "No user message found");
        return;
    }

    // Get session ID or generate one
    char* session_id = NULL;
    if (session_id_json && session_id_json->valuestring) {
        session_id = strdup(session_id_json->valuestring);
    } else {
        session_id = malloc(64);
        snprintf(session_id, 64, "session_%ld", time(NULL));
    }

    log_debug("[ACP] Chat completion request, session: %s, message: %s", session_id, user_message);

    // For now, return a simple response since we can't easily call the agent loop synchronously
    // In a full implementation, this would inject a message into the bus and wait for response
    char* response_id = generate_response_id();
    const char* model = model_json && model_json->valuestring ? model_json->valuestring : "primagen-default";

    // Simple acknowledgment response
    char response_content[1024];
    snprintf(response_content, sizeof(response_content),
        "Message received. Session: %s. In a full implementation, this would process through the agent loop.",
        session_id);

    char* response_json = acp_chat_response_json(response_id, model, response_content, "stop");

    send_json_response(nc, 200, response_json);

    // Cleanup
    free(response_json);
    free(response_id);
    free(session_id);
    cJSON_Delete(root);
}

// Generate tools list JSON
char* acp_tools_list_json(ToolRegistry* registry) {
    if (!registry) return NULL;

    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON* tools = cJSON_CreateArray();
    if (!tools) {
        cJSON_Delete(root);
        return NULL;
    }

    // Iterate over tools in registry
    for (size_t i = 0; i < registry->count; i++) {
        Tool* tool = &registry->tools[i];
        if (!tool) continue;

        cJSON* tool_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_obj, "name", tool->def.name.data);
        cJSON_AddStringToObject(tool_obj, "description", tool->def.description.data);

        // Add input schema
        cJSON* input_schema = cJSON_Parse(tool->def.parameters.data);
        if (input_schema) {
            cJSON_AddItemToObject(tool_obj, "input_schema", input_schema);
        }

        cJSON_AddItemToArray(tools, tool_obj);
    }

    cJSON_AddItemToObject(root, "tools", tools);
    cJSON_AddBoolToObject(root, "success", true);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

// Generate tool call response JSON
char* acp_tool_response_json(const char* tool_name, const char* result, const char* error) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "tool_name", tool_name);

    if (error) {
        cJSON_AddStringToObject(root, "error", error);
        cJSON_AddBoolToObject(root, "success", false);
    } else {
        // Try to parse result as JSON, otherwise add as string
        cJSON* result_json = cJSON_Parse(result);
        if (result_json) {
            cJSON_AddItemToObject(root, "result", result_json);
        } else {
            cJSON_AddStringToObject(root, "result", result);
        }
        cJSON_AddBoolToObject(root, "success", true);
    }

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

// Generate chat completion response JSON
char* acp_chat_response_json(const char* id, const char* model, const char* content, const char* finish_reason) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddStringToObject(root, "object", "chat.completion");
    cJSON_AddNumberToObject(root, "created", time(NULL));
    cJSON_AddStringToObject(root, "model", model);

    cJSON* choices = cJSON_CreateArray();
    cJSON* choice = cJSON_CreateObject();
    cJSON_AddNumberToObject(choice, "index", 0);

    cJSON* message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "assistant");
    cJSON_AddStringToObject(message, "content", content);
    cJSON_AddItemToObject(choice, "message", message);

    cJSON_AddStringToObject(choice, "finish_reason", finish_reason);
    cJSON_AddItemToArray(choices, choice);
    cJSON_AddItemToObject(root, "choices", choices);

    // Usage stats (placeholder)
    cJSON* usage = cJSON_CreateObject();
    cJSON_AddNumberToObject(usage, "prompt_tokens", 0);
    cJSON_AddNumberToObject(usage, "completion_tokens", 0);
    cJSON_AddNumberToObject(usage, "total_tokens", 0);
    cJSON_AddItemToObject(root, "usage", usage);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

// Generate error response JSON
char* acp_error_json(int code, const char* message) {
    cJSON* root = cJSON_CreateObject();

    cJSON* error = cJSON_CreateObject();
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);

    cJSON_AddItemToObject(root, "error", error);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}
