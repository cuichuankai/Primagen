#include "mcp.h"
#include "transport_internal.h"
#include "../include/logger.h"
#include "../vendor/cJSON/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

// Request timeout in milliseconds
#define MCP_REQUEST_TIMEOUT 30000

// Next request ID
static long g_next_request_id = 1;
static pthread_mutex_t g_request_id_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    const char* type;
    MCPTransportOps* (*ops_provider)(void);
} MCPTransportRegistryEntry;

static void mcp_free_tools(MCPClient* client) {
    if (!client || !client->tools) return;
    for (size_t j = 0; j < client->tools_count; j++) {
        free(client->tools[j].name);
        free(client->tools[j].description);
        string_free(&client->tools[j].input_schema);
    }
    free(client->tools);
    client->tools = NULL;
    client->tools_count = 0;
}

static void mcp_free_resources(MCPClient* client) {
    if (!client || !client->resources) return;
    for (size_t j = 0; j < client->resources_count; j++) {
        free(client->resources[j].uri);
        free(client->resources[j].name);
        free(client->resources[j].description);
        free(client->resources[j].mime_type);
    }
    free(client->resources);
    client->resources = NULL;
    client->resources_count = 0;
}

static void mcp_free_prompts(MCPClient* client) {
    if (!client || !client->prompts) return;
    for (size_t j = 0; j < client->prompts_count; j++) {
        free(client->prompts[j].name);
        free(client->prompts[j].description);
        string_free(&client->prompts[j].arguments);
    }
    free(client->prompts);
    client->prompts = NULL;
    client->prompts_count = 0;
}

static long get_next_request_id() {
    pthread_mutex_lock(&g_request_id_mutex);
    long id = g_next_request_id++;
    pthread_mutex_unlock(&g_request_id_mutex);
    return id;
}

static MCPTransportOps* get_transport_ops(const char* type) {
    if (!type || type[0] == '\0') return NULL;

    static MCPTransportRegistryEntry registry[] = {
        {"stdio", mcp_transport_stdio_ops},
        {"websocket", mcp_transport_websocket_ops},
        {"sse", mcp_transport_sse_ops},
        {"streamable_http", mcp_transport_streamable_http_ops},
    };
    size_t count = sizeof(registry) / sizeof(registry[0]);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(type, registry[i].type) == 0) {
            return registry[i].ops_provider();
        }
    }
    return NULL;
}

Error mcp_client_connect(MCPClient* client) {
    if (!client) return error_new(ERR_INVALID_PARAM, "client is NULL");
    log_debug("[MCP] Connecting to %s via %s...", client->server_id, client->transport_type);
    MCPTransportOps* ops = get_transport_ops(client->transport_type);
    if (!ops) {
        log_error("[MCP] Unknown transport type: %s", client->transport_type);
        return error_new(ERR_INVALID_PARAM, "Unknown transport type");
    }
    Error err = ops->init(client);
    if (err.code == ERR_NONE) {
        client->connected = true;
        log_debug("[MCP] Connected to %s", client->server_id);
    }
    return err;
}

void mcp_client_disconnect(MCPClient* client) {
    if (!client) return;
    log_debug("[MCP] Disconnecting from %s", client->server_id);
    MCPTransportOps* ops = get_transport_ops(client->transport_type);
    if (ops && ops->close) ops->close(client);
    client->connected = false;
}

Error mcp_client_send(MCPClient* client, const char* request_json) {
    if (!client || !request_json) return error_new(ERR_INVALID_PARAM, "Invalid arguments");
    MCPTransportOps* ops = get_transport_ops(client->transport_type);
    if (!ops || !ops->send) return error_new(ERR_INVALID_PARAM, "Transport not available");
    return ops->send(client, request_json);
}

char* mcp_client_recv(MCPClient* client, int timeout_ms) {
    if (!client) return NULL;
    MCPTransportOps* ops = get_transport_ops(client->transport_type);
    if (!ops || !ops->recv) return NULL;
    return ops->recv(client, timeout_ms);
}

// MCP method string mapping
static const char* method_strings[] = {
    "invalid",
    "initialize",
    "notifications/initialized",
    "tools/list",
    "tools/call",
    "resources/list",
    "resources/read",
    "prompts/list",
    "prompts/get"
};

const char* mcp_method_to_string(MCPMethod method) {
    if (method < 0 || method > MCP_METHOD_PROMPTS_GET) {
        return method_strings[0];
    }
    return method_strings[method];
}

MCPMethod mcp_method_from_string(const char* str) {
    if (!str) return MCP_METHOD_INVALID;

    if (strcmp(str, "initialize") == 0) return MCP_METHOD_INITIALIZE;
    if (strcmp(str, "notifications/initialized") == 0) return MCP_METHOD_INITIALIZED;
    if (strcmp(str, "tools/list") == 0) return MCP_METHOD_TOOLS_LIST;
    if (strcmp(str, "tools/call") == 0) return MCP_METHOD_TOOLS_CALL;
    if (strcmp(str, "resources/list") == 0) return MCP_METHOD_RESOURCES_LIST;
    if (strcmp(str, "resources/read") == 0) return MCP_METHOD_RESOURCES_READ;
    if (strcmp(str, "prompts/list") == 0) return MCP_METHOD_PROMPTS_LIST;
    if (strcmp(str, "prompts/get") == 0) return MCP_METHOD_PROMPTS_GET;

    return MCP_METHOD_INVALID;
}

// MCP Manager creation
MCPManager* mcp_manager_create(const char* workspace) {
    if (!workspace) return NULL;

    MCPManager* mgr = malloc(sizeof(MCPManager));
    if (!mgr) return NULL;

    mgr->clients = NULL;
    mgr->clients_count = 0;
    mgr->clients_capacity = 0;
    mgr->workspace = strdup(workspace);
    if (!mgr->workspace) {
        free(mgr);
        return NULL;
    }

    return mgr;
}

void mcp_manager_free(MCPManager* mgr) {
    if (!mgr) return;

    // Free all clients
    for (size_t i = 0; i < mgr->clients_count; i++) {
        MCPClient* client = mgr->clients[i];
        if (client) {
            if (client->connected) {
                mcp_client_disconnect(client);
            }
            free(client->server_id);
            free(client->transport_type);
            free(client->command);

            // Free args
            for (size_t j = 0; j < client->args_count; j++) {
                free(client->args[j]);
            }
            free(client->args);

            // Free environment variables
            for (size_t j = 0; j < client->env.count; j++) {
                free(client->env.items[j].key);
                free(client->env.items[j].value);
            }
            free(client->env.items);

            mcp_free_tools(client);
            mcp_free_resources(client);
            mcp_free_prompts(client);

            free(client);
        }
    }
    free(mgr->clients);
    free(mgr->workspace);
    free(mgr);
}

// Add a new MCP client
int mcp_manager_add_client(MCPManager* mgr, const char* server_id, const char* transport,
                           const char* command, char** args, size_t args_count,
                           EnvVar* env_vars, size_t env_count) {
    if (!mgr || !server_id || !transport) return -1;

    // Check if client already exists
    for (size_t i = 0; i < mgr->clients_count; i++) {
        if (strcmp(mgr->clients[i]->server_id, server_id) == 0) {
            log_error("[MCP] Client %s already exists", server_id);
            return -1;
        }
    }

    // Grow array if needed
    if (mgr->clients_count >= mgr->clients_capacity) {
        size_t new_capacity = mgr->clients_capacity == 0 ? 4 : mgr->clients_capacity * 2;
        MCPClient** new_clients = realloc(mgr->clients, new_capacity * sizeof(MCPClient*));
        if (!new_clients) return -1;
        mgr->clients = new_clients;
        mgr->clients_capacity = new_capacity;
    }

    // Create client
    MCPClient* client = malloc(sizeof(MCPClient));
    if (!client) return -1;
    memset(client, 0, sizeof(MCPClient));

    client->server_id = strdup(server_id);
    if (!client->server_id) {
        free(client);
        return -1;
    }

    client->transport_type = strdup(transport);
    if (!client->transport_type) {
        free(client->server_id);
        free(client);
        return -1;
    }

    if (command) {
        client->command = strdup(command);
        if (!client->command) {
            free(client->transport_type);
            free(client->server_id);
            free(client);
            return -1;
        }
    }

    // Copy args
    if (args && args_count > 0) {
        client->args = malloc(args_count * sizeof(char*));
        if (!client->args) {
            free(client->command);
            free(client->transport_type);
            free(client->server_id);
            free(client);
            return -1;
        }
        client->args_count = args_count;
        for (size_t i = 0; i < args_count; i++) {
            client->args[i] = strdup(args[i]);
            if (!client->args[i]) {
                // Free already allocated args
                for (size_t j = 0; j < i; j++) {
                    free(client->args[j]);
                }
                free(client->args);
                free(client->command);
                free(client->transport_type);
                free(client->server_id);
                free(client);
                return -1;
            }
        }
    }

    // Copy environment variables
    if (env_vars && env_count > 0) {
        client->env.items = malloc(env_count * sizeof(EnvVar));
        if (!client->env.items) {
            if (client->args) {
                for (size_t i = 0; i < client->args_count; i++) {
                    free(client->args[i]);
                }
                free(client->args);
            }
            free(client->command);
            free(client->transport_type);
            free(client->server_id);
            free(client);
            return -1;
        }
        client->env.count = env_count;
        for (size_t i = 0; i < env_count; i++) {
            client->env.items[i].key = strdup(env_vars[i].key);
            if (!client->env.items[i].key) {
                // Free already allocated keys
                for (size_t j = 0; j < i; j++) {
                    free(client->env.items[j].key);
                    free(client->env.items[j].value);
                }
                free(client->env.items);
                if (client->args) {
                    for (size_t j = 0; j < client->args_count; j++) {
                        free(client->args[j]);
                    }
                    free(client->args);
                }
                free(client->command);
                free(client->transport_type);
                free(client->server_id);
                free(client);
                return -1;
            }
            client->env.items[i].value = strdup(env_vars[i].value);
            if (!client->env.items[i].value) {
                free(client->env.items[i].key);
                // Free already allocated env vars
                for (size_t j = 0; j < i; j++) {
                    free(client->env.items[j].key);
                    free(client->env.items[j].value);
                }
                free(client->env.items);
                if (client->args) {
                    for (size_t j = 0; j < client->args_count; j++) {
                        free(client->args[j]);
                    }
                    free(client->args);
                }
                free(client->command);
                free(client->transport_type);
                free(client->server_id);
                free(client);
                return -1;
            }
        }
    }

    client->connected = false;
    client->initialized = false;
    client->tools = NULL;
    client->tools_count = 0;
    client->resources = NULL;
    client->resources_count = 0;
    client->prompts = NULL;
    client->prompts_count = 0;

    mgr->clients[mgr->clients_count++] = client;

    log_debug("[MCP] Added client: %s (transport: %s)", server_id, transport);
    return (int)(mgr->clients_count - 1);
}

void mcp_manager_remove_client(MCPManager* mgr, const char* server_id) {
    if (!mgr || !server_id) return;

    for (size_t i = 0; i < mgr->clients_count; i++) {
        if (strcmp(mgr->clients[i]->server_id, server_id) == 0) {
            MCPClient* client = mgr->clients[i];
            if (client->connected) {
                mcp_client_disconnect(client);
            }

            // Remove from array
            for (size_t j = i; j < mgr->clients_count - 1; j++) {
                mgr->clients[j] = mgr->clients[j + 1];
            }
            mgr->clients_count--;

            // Free client (same as in mcp_manager_free)
            free(client->server_id);
            free(client->transport_type);
            free(client->command);
            for (size_t j = 0; j < client->args_count; j++) {
                free(client->args[j]);
            }
            free(client->args);
            mcp_free_tools(client);
            mcp_free_resources(client);
            mcp_free_prompts(client);
            free(client);

            log_debug("[MCP] Removed client: %s", server_id);
            return;
        }
    }
}

MCPClient* mcp_manager_get_client(MCPManager* mgr, const char* server_id) {
    if (!mgr || !server_id) return NULL;

    for (size_t i = 0; i < mgr->clients_count; i++) {
        if (strcmp(mgr->clients[i]->server_id, server_id) == 0) {
            return mgr->clients[i];
        }
    }
    return NULL;
}

Error mcp_manager_setup_from_config(MCPManager* mgr, MCPConfig* cfg, ToolRegistry* tool_reg) {
    if (!mgr || !cfg || !tool_reg) {
        return error_new(ERR_INVALID_PARAM, "Invalid MCP manager setup args");
    }

    for (size_t i = 0; i < cfg->server_count; i++) {
        MCPServerConfig* srv = &cfg->servers[i];
        log_debug("[MCP] Adding server: %s (transport: %s)", srv->server_id, srv->transport_type);

        char** args = NULL;
        if (srv->args.count > 0) {
            args = malloc(srv->args.count * sizeof(char*));
            if (!args) {
                return error_new(ERR_MEMORY, "Failed to allocate MCP args");
            }
            for (size_t j = 0; j < srv->args.count; j++) {
                args[j] = srv->args.items[j].data;
            }
        }

        int add_idx = mcp_manager_add_client(mgr, srv->server_id, srv->transport_type,
                                             srv->command, args, srv->args.count,
                                             srv->env.items, srv->env.count);
        if (args) free(args);
        if (add_idx < 0) {
            log_error("[MCP] Failed to add server: %s", srv->server_id);
        }
    }

    for (size_t i = 0; i < mgr->clients_count; i++) {
        MCPClient* client = mgr->clients[i];
        Error err = mcp_client_connect(client);
        if (err.code == ERR_NONE) {
            log_debug("[MCP] Connected to %s", client->server_id);

            MCPToolDef* tools = NULL;
            size_t tools_count = 0;
            err = mcp_client_list_tools(client, &tools, &tools_count);
            if (err.code == ERR_NONE && tools_count > 0) {
                log_debug("[MCP] %s provides %zu tools", client->server_id, tools_count);
                mcp_register_tools(tool_reg, client);
                for (size_t j = 0; j < tools_count; j++) {
                    log_debug("  - Tool: %s", tools[j].name);
                }
            } else if (err.code != ERR_NONE) {
                if (err.message[0] == '\0') {
                    log_error("[MCP] Failed to list tools from %s: unknown error (code=%d)", client->server_id, err.code);
                } else {
                    log_error("[MCP] Failed to list tools from %s: %s", client->server_id, err.message);
                }
            } else {
                log_debug("[MCP] %s provides no tools", client->server_id);
            }

            mcp_register_resources_prompts(tool_reg, client);
        } else {
            log_error("[MCP] Failed to connect to %s: %s", client->server_id, err.message);
        }
    }

    return error_new(ERR_NONE, "");
}

// Connection management is implemented in this file via transport registry
// void mcp_client_disconnect(MCPClient* client);

static Error mcp_client_initialize(MCPClient* client) {
    if (!client) return error_new(ERR_INVALID_PARAM, "Invalid client");
    if (!client->connected) return error_new(ERR_TOOL, "Client not connected");
    if (client->initialized) return error_new(ERR_NONE, "");

    cJSON* params_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(params_obj, "protocolVersion", MCP_PROTOCOL_VERSION);
    cJSON_AddItemToObject(params_obj, "capabilities", cJSON_CreateObject());
    cJSON* client_info = cJSON_CreateObject();
    cJSON_AddStringToObject(client_info, "name", "primagen");
    cJSON_AddStringToObject(client_info, "version", "0.1.0");
    cJSON_AddItemToObject(params_obj, "clientInfo", client_info);
    char* params_str = cJSON_PrintUnformatted(params_obj);
    cJSON_Delete(params_obj);

    if (!params_str) {
        return error_new(ERR_JSON, "Failed to create initialize params");
    }

    long request_id = get_next_request_id();
    char* request = mcp_create_request(MCP_METHOD_INITIALIZE, request_id, params_str);
    free(params_str);
    if (!request) {
        return error_new(ERR_JSON, "Failed to create initialize request");
    }

    Error err = mcp_client_send(client, request);
    free(request);
    if (err.code != ERR_NONE) {
        return err;
    }

    char* response_json = mcp_client_recv(client, MCP_REQUEST_TIMEOUT);
    if (!response_json) {
        return error_new(ERR_TIMEOUT, "No initialize response from server");
    }

    MCPResponse response;
    memset(&response, 0, sizeof(response));
    char* parse_error = mcp_parse_response(response_json, &response);
    free(response_json);

    if (parse_error) {
        return error_new(ERR_JSON, parse_error);
    }
    if (response.error.data && strlen(response.error.data) > 0) {
        mcp_response_free(&response);
        return error_new(ERR_TOOL, "Initialize failed");
    }

    cJSON* notify = cJSON_CreateObject();
    cJSON_AddStringToObject(notify, "jsonrpc", "2.0");
    cJSON_AddStringToObject(notify, "method", "notifications/initialized");
    cJSON_AddItemToObject(notify, "params", cJSON_CreateObject());
    char* notify_str = cJSON_PrintUnformatted(notify);
    cJSON_Delete(notify);

    if (!notify_str) {
        mcp_response_free(&response);
        return error_new(ERR_JSON, "Failed to create initialized notification");
    }

    Error notify_err = mcp_client_send(client, notify_str);
    free(notify_str);
    if (notify_err.code != ERR_NONE) {
        mcp_response_free(&response);
        return notify_err;
    }

    mcp_response_free(&response);
    client->initialized = true;
    log_debug("[MCP] Initialized client: %s", client->server_id);
    return error_new(ERR_NONE, "");
}

// List tools from MCP server
Error mcp_client_list_tools(MCPClient* client, MCPToolDef** tools, size_t* count) {
    if (!client || !tools || !count) return error_new(ERR_INVALID_PARAM, "Invalid arguments");
    if (!client->connected) return error_new(ERR_TOOL, "Client not connected");

    Error init_err = mcp_client_initialize(client);
    if (init_err.code != ERR_NONE) return init_err;

    log_debug("[MCP] Listing tools from %s...", client->server_id);

    // Create tools/list request
    long request_id = get_next_request_id();
    char* request = mcp_create_request(MCP_METHOD_TOOLS_LIST, request_id, "{}");
    if (!request) {
        return error_new(ERR_JSON, "Failed to create request");
    }

    // Send request
    Error err = mcp_client_send(client, request);
    free(request);
    if (err.code != ERR_NONE) {
        return err;
    }

    // Receive response
    char* response_json = mcp_client_recv(client, MCP_REQUEST_TIMEOUT);
    if (!response_json) {
        return error_new(ERR_TIMEOUT, "No response from server");
    }

    // Parse response
    MCPResponse response;
    memset(&response, 0, sizeof(response));
    char* parse_error = mcp_parse_response(response_json, &response);
    free(response_json);

    if (parse_error) {
        return error_new(ERR_JSON, parse_error);
    }

    if (response.error.data && strlen(response.error.data) > 0) {
        const char* err_msg = response.error.data;
        if (strspn(err_msg, " \t\r\n") == strlen(err_msg)) {
            err_msg = "Server returned empty MCP error";
        }
        char err_copy[256];
        strncpy(err_copy, err_msg, sizeof(err_copy) - 1);
        err_copy[sizeof(err_copy) - 1] = '\0';
        mcp_response_free(&response);
        return error_new(ERR_TOOL, err_copy);
    }

    // Parse tools from result
    cJSON* result = cJSON_Parse(response.result.data);
    if (!result) {
        mcp_response_free(&response);
        return error_new(ERR_JSON, "Failed to parse result");
    }

    cJSON* tools_array = cJSON_GetObjectItem(result, "tools");
    if (!cJSON_IsArray(tools_array)) {
        cJSON_Delete(result);
        mcp_response_free(&response);
        return error_new(ERR_JSON, "No tools array in response");
    }

    *count = cJSON_GetArraySize(tools_array);
    if (*count == 0) {
        *tools = NULL;
        cJSON_Delete(result);
        mcp_response_free(&response);
        return error_new(ERR_NONE, "");
    }

    *tools = calloc(*count, sizeof(MCPToolDef));

    cJSON* tool_item;
    size_t idx = 0;
    cJSON_ArrayForEach(tool_item, tools_array) {
        cJSON* name = cJSON_GetObjectItem(tool_item, "name");
        cJSON* desc = cJSON_GetObjectItem(tool_item, "description");
        cJSON* schema = cJSON_GetObjectItem(tool_item, "inputSchema");

        if (cJSON_IsString(name)) {
            (*tools)[idx].name = strdup(name->valuestring);
        }
        if (cJSON_IsString(desc)) {
            (*tools)[idx].description = strdup(desc->valuestring);
        }
        if (schema) {
            char* schema_str = cJSON_PrintUnformatted(schema);
            (*tools)[idx].input_schema = string_new(schema_str);
            free(schema_str);
        }
        idx++;
    }

    mcp_free_tools(client);
    client->tools = *tools;
    client->tools_count = *count;

    cJSON_Delete(result);
    mcp_response_free(&response);

    log_debug("[MCP] Listed %zu tools from %s", *count, client->server_id);
    return error_new(ERR_NONE, "");
}

// Call tool on MCP server
Error mcp_client_call_tool(MCPClient* client, const char* name, const char* params,
                           String* result) {
    if (!client || !name || !result) return error_new(ERR_INVALID_PARAM, "Invalid arguments");
    if (!client->connected) return error_new(ERR_TOOL, "Client not connected");

    log_debug("[MCP] Calling tool %s on %s...", name, client->server_id);

    // Build params JSON
    cJSON* params_obj = NULL;
    if (params) {
        params_obj = cJSON_Parse(params);
        if (!params_obj) {
            return error_new(ERR_JSON, "Invalid params JSON");
        }
    } else {
        params_obj = cJSON_CreateObject();
    }

    cJSON* call_params = cJSON_CreateObject();
    cJSON_AddStringToObject(call_params, "name", name);
    cJSON_AddItemToObject(call_params, "arguments", params_obj);

    char* params_str = cJSON_PrintUnformatted(call_params);
    cJSON_Delete(call_params);

    // Create tools/call request
    long request_id = get_next_request_id();
    char* request = mcp_create_request(MCP_METHOD_TOOLS_CALL, request_id, params_str);
    free(params_str);

    if (!request) {
        return error_new(ERR_JSON, "Failed to create request");
    }

    // Send request
    Error err = mcp_client_send(client, request);
    free(request);
    if (err.code != ERR_NONE) {
        return err;
    }

    // Receive response
    char* response_json = mcp_client_recv(client, MCP_REQUEST_TIMEOUT);
    if (!response_json) {
        return error_new(ERR_TIMEOUT, "No response from server");
    }

    // Parse response
    MCPResponse response;
    memset(&response, 0, sizeof(response));
    char* parse_error = mcp_parse_response(response_json, &response);
    free(response_json);

    if (parse_error) {
        return error_new(ERR_JSON, parse_error);
    }

    if (response.error.data && strlen(response.error.data) > 0) {
        mcp_response_free(&response);
        return error_new(ERR_TOOL, "Server returned error");
    }

    // Extract result
    cJSON* result_json = cJSON_Parse(response.result.data);
    if (result_json) {
        cJSON* content = cJSON_GetObjectItem(result_json, "content");
        if (content) {
            char* content_str = cJSON_PrintUnformatted(content);
            *result = string_new(content_str);
            free(content_str);
        } else {
            *result = string_new(response.result.data);
        }
        cJSON_Delete(result_json);
    } else {
        *result = string_new(response.result.data);
    }

    mcp_response_free(&response);

    log_debug("[MCP] Tool %s completed", name);
    return error_new(ERR_NONE, "");
}

// List resources from MCP server
Error mcp_client_list_resources(MCPClient* client, MCPResource** resources, size_t* count) {
    if (!client || !resources || !count) return error_new(ERR_INVALID_PARAM, "Invalid arguments");
    if (!client->connected) return error_new(ERR_TOOL, "Client not connected");

    log_debug("[MCP] Listing resources from %s...", client->server_id);

    // Create resources/list request
    long request_id = get_next_request_id();
    char* request = mcp_create_request(MCP_METHOD_RESOURCES_LIST, request_id, "{}");
    if (!request) {
        return error_new(ERR_JSON, "Failed to create request");
    }

    // Send request
    Error err = mcp_client_send(client, request);
    free(request);
    if (err.code != ERR_NONE) {
        return err;
    }

    // Receive response
    char* response_json = mcp_client_recv(client, MCP_REQUEST_TIMEOUT);
    if (!response_json) {
        return error_new(ERR_TIMEOUT, "No response from server");
    }

    // Parse response
    MCPResponse response;
    memset(&response, 0, sizeof(response));
    char* parse_error = mcp_parse_response(response_json, &response);
    free(response_json);

    if (parse_error) {
        return error_new(ERR_JSON, parse_error);
    }

    if (response.error.data && strlen(response.error.data) > 0) {
        mcp_response_free(&response);
        return error_new(ERR_TOOL, "Server returned error");
    }

    // Parse resources from result
    cJSON* result = cJSON_Parse(response.result.data);
    if (!result) {
        mcp_response_free(&response);
        return error_new(ERR_JSON, "Failed to parse result");
    }

    cJSON* resources_array = cJSON_GetObjectItem(result, "resources");
    if (!cJSON_IsArray(resources_array)) {
        cJSON_Delete(result);
        mcp_response_free(&response);
        return error_new(ERR_JSON, "No resources array in response");
    }

    *count = cJSON_GetArraySize(resources_array);
    if (*count == 0) {
        *resources = NULL;
        cJSON_Delete(result);
        mcp_response_free(&response);
        return error_new(ERR_NONE, "");
    }

    *resources = calloc(*count, sizeof(MCPResource));

    cJSON* res_item;
    size_t idx = 0;
    cJSON_ArrayForEach(res_item, resources_array) {
        cJSON* uri = cJSON_GetObjectItem(res_item, "uri");
        cJSON* name = cJSON_GetObjectItem(res_item, "name");
        cJSON* desc = cJSON_GetObjectItem(res_item, "description");
        cJSON* mime = cJSON_GetObjectItem(res_item, "mimeType");

        if (cJSON_IsString(uri)) (*resources)[idx].uri = strdup(uri->valuestring);
        if (cJSON_IsString(name)) (*resources)[idx].name = strdup(name->valuestring);
        if (cJSON_IsString(desc)) (*resources)[idx].description = strdup(desc->valuestring);
        if (cJSON_IsString(mime)) (*resources)[idx].mime_type = strdup(mime->valuestring);
        idx++;
    }

    mcp_free_resources(client);
    client->resources = *resources;
    client->resources_count = *count;

    cJSON_Delete(result);
    mcp_response_free(&response);

    log_debug("[MCP] Listed %zu resources from %s", *count, client->server_id);
    return error_new(ERR_NONE, "");
}

// Read resource from MCP server
Error mcp_client_read_resource(MCPClient* client, const char* uri, String* content) {
    if (!client || !uri || !content) return error_new(ERR_INVALID_PARAM, "Invalid arguments");
    if (!client->connected) return error_new(ERR_TOOL, "Client not connected");

    log_debug("[MCP] Reading resource %s from %s...", uri, client->server_id);

    // Build params
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "uri", uri);
    char* params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);

    // Create resources/read request
    long request_id = get_next_request_id();
    char* request = mcp_create_request(MCP_METHOD_RESOURCES_READ, request_id, params_str);
    free(params_str);

    if (!request) {
        return error_new(ERR_JSON, "Failed to create request");
    }

    // Send request
    Error err = mcp_client_send(client, request);
    free(request);
    if (err.code != ERR_NONE) {
        return err;
    }

    // Receive response
    char* response_json = mcp_client_recv(client, MCP_REQUEST_TIMEOUT);
    if (!response_json) {
        return error_new(ERR_TIMEOUT, "No response from server");
    }

    // Parse response
    MCPResponse response;
    memset(&response, 0, sizeof(response));
    char* parse_error = mcp_parse_response(response_json, &response);
    free(response_json);

    if (parse_error) {
        return error_new(ERR_JSON, parse_error);
    }

    if (response.error.data && strlen(response.error.data) > 0) {
        mcp_response_free(&response);
        return error_new(ERR_TOOL, "Server returned error");
    }

    // Extract content
    cJSON* result = cJSON_Parse(response.result.data);
    if (result) {
        cJSON* contents = cJSON_GetObjectItem(result, "contents");
        if (cJSON_IsArray(contents) && cJSON_GetArraySize(contents) > 0) {
            cJSON* first = cJSON_GetArrayItem(contents, 0);
            cJSON* text = cJSON_GetObjectItem(first, "text");
            if (cJSON_IsString(text)) {
                *content = string_new(text->valuestring);
            }
        }
        cJSON_Delete(result);
    }

    mcp_response_free(&response);

    log_debug("[MCP] Read resource %s completed", uri);
    return error_new(ERR_NONE, "");
}

// List prompts from MCP server
Error mcp_client_list_prompts(MCPClient* client, MCPPrompt** prompts, size_t* count) {
    if (!client || !prompts || !count) return error_new(ERR_INVALID_PARAM, "Invalid arguments");
    if (!client->connected) return error_new(ERR_TOOL, "Client not connected");

    log_debug("[MCP] Listing prompts from %s...", client->server_id);

    // Create prompts/list request
    long request_id = get_next_request_id();
    char* request = mcp_create_request(MCP_METHOD_PROMPTS_LIST, request_id, "{}");
    if (!request) {
        return error_new(ERR_JSON, "Failed to create request");
    }

    // Send request
    Error err = mcp_client_send(client, request);
    free(request);
    if (err.code != ERR_NONE) {
        return err;
    }

    // Receive response
    char* response_json = mcp_client_recv(client, MCP_REQUEST_TIMEOUT);
    if (!response_json) {
        return error_new(ERR_TIMEOUT, "No response from server");
    }

    // Parse response
    MCPResponse response;
    memset(&response, 0, sizeof(response));
    char* parse_error = mcp_parse_response(response_json, &response);
    free(response_json);

    if (parse_error) {
        return error_new(ERR_JSON, parse_error);
    }

    if (response.error.data && strlen(response.error.data) > 0) {
        mcp_response_free(&response);
        return error_new(ERR_TOOL, "Server returned error");
    }

    // Parse prompts from result
    cJSON* result = cJSON_Parse(response.result.data);
    if (!result) {
        mcp_response_free(&response);
        return error_new(ERR_JSON, "Failed to parse result");
    }

    cJSON* prompts_array = cJSON_GetObjectItem(result, "prompts");
    if (!cJSON_IsArray(prompts_array)) {
        cJSON_Delete(result);
        mcp_response_free(&response);
        return error_new(ERR_JSON, "No prompts array in response");
    }

    *count = cJSON_GetArraySize(prompts_array);
    if (*count == 0) {
        *prompts = NULL;
        cJSON_Delete(result);
        mcp_response_free(&response);
        return error_new(ERR_NONE, "");
    }

    *prompts = calloc(*count, sizeof(MCPPrompt));

    cJSON* prompt_item;
    size_t idx = 0;
    cJSON_ArrayForEach(prompt_item, prompts_array) {
        cJSON* name = cJSON_GetObjectItem(prompt_item, "name");
        cJSON* desc = cJSON_GetObjectItem(prompt_item, "description");
        cJSON* args = cJSON_GetObjectItem(prompt_item, "arguments");

        if (cJSON_IsString(name)) (*prompts)[idx].name = strdup(name->valuestring);
        if (cJSON_IsString(desc)) (*prompts)[idx].description = strdup(desc->valuestring);
        if (args) {
            char* args_str = cJSON_PrintUnformatted(args);
            (*prompts)[idx].arguments = string_new(args_str);
            free(args_str);
        }
        idx++;
    }

    mcp_free_prompts(client);
    client->prompts = *prompts;
    client->prompts_count = *count;

    cJSON_Delete(result);
    mcp_response_free(&response);

    log_debug("[MCP] Listed %zu prompts from %s", *count, client->server_id);
    return error_new(ERR_NONE, "");
}

// Get prompt from MCP server
Error mcp_client_get_prompt(MCPClient* client, const char* name, const char* args,
                            String* result) {
    if (!client || !name || !result) return error_new(ERR_INVALID_PARAM, "Invalid arguments");
    if (!client->connected) return error_new(ERR_TOOL, "Client not connected");

    log_debug("[MCP] Getting prompt %s from %s...", name, client->server_id);

    // Build params
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", name);
    if (args) {
        cJSON* args_json = cJSON_Parse(args);
        if (args_json) {
            cJSON_AddItemToObject(params, "arguments", args_json);
        }
    }
    char* params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);

    // Create prompts/get request
    long request_id = get_next_request_id();
    char* request = mcp_create_request(MCP_METHOD_PROMPTS_GET, request_id, params_str);
    free(params_str);

    if (!request) {
        return error_new(ERR_JSON, "Failed to create request");
    }

    // Send request
    Error err = mcp_client_send(client, request);
    free(request);
    if (err.code != ERR_NONE) {
        return err;
    }

    // Receive response
    char* response_json = mcp_client_recv(client, MCP_REQUEST_TIMEOUT);
    if (!response_json) {
        return error_new(ERR_TIMEOUT, "No response from server");
    }

    // Parse response
    MCPResponse response;
    memset(&response, 0, sizeof(response));
    char* parse_error = mcp_parse_response(response_json, &response);
    free(response_json);

    if (parse_error) {
        return error_new(ERR_JSON, parse_error);
    }

    if (response.error.data && strlen(response.error.data) > 0) {
        mcp_response_free(&response);
        return error_new(ERR_TOOL, "Server returned error");
    }

    // Extract result
    *result = string_new(response.result.data);

    mcp_response_free(&response);

    log_debug("[MCP] Got prompt %s", name);
    return error_new(ERR_NONE, "");
}

// Create JSON-RPC request
char* mcp_create_request(MCPMethod method, long id, const char* params_json) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddStringToObject(root, "method", mcp_method_to_string(method));

    if (params_json) {
        cJSON* params = cJSON_Parse(params_json);
        if (params) {
            cJSON_AddItemToObject(root, "params", params);
        }
    }

    char* result = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return result;
}

// Parse JSON-RPC response
char* mcp_parse_response(const char* json, MCPResponse* response) {
    if (!json || !response) return NULL;

    cJSON* root = cJSON_Parse(json);
    if (!root) return "Failed to parse JSON";

    cJSON* item = cJSON_GetObjectItem(root, "jsonrpc");
    if (!item || !cJSON_IsString(item) || strcmp(item->valuestring, "2.0") != 0) {
        cJSON_Delete(root);
        return "Invalid JSON-RPC version";
    }

    item = cJSON_GetObjectItem(root, "id");
    response->id = (item && cJSON_IsNumber(item)) ? item->valueint : 0;

    item = cJSON_GetObjectItem(root, "result");
    if (item) {
        char* result_str = cJSON_PrintUnformatted(item);
        response->result = string_new(result_str);
        free(result_str);
    } else {
        response->result = string_new("");
    }

    item = cJSON_GetObjectItem(root, "error");
    if (item) {
        char* error_str = cJSON_PrintUnformatted(item);
        response->error = string_new(error_str);
        free(error_str);
    } else {
        response->error = string_new("");
    }

    cJSON_Delete(root);
    return NULL;
}

void mcp_response_free(MCPResponse* response) {
    if (!response) return;
    string_free(&response->result);
    string_free(&response->error);
}
