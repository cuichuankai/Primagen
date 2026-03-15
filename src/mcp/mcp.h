#ifndef MCP_H
#define MCP_H

#include "../include/common.h"
#include "../tools/tool.h"

// MCP protocol constants
#define MCP_VERSION "2024-11-05"
#define MCP_PROTOCOL_VERSION "2024-11-05"

// MCP message types
typedef enum {
    MCP_MESSAGE_INVALID = 0,
    MCP_MESSAGE_REQUEST,
    MCP_MESSAGE_RESPONSE,
    MCP_MESSAGE_NOTIFICATION
} MCPMessageType;

// MCP request methods
typedef enum {
    MCP_METHOD_INVALID = 0,
    MCP_METHOD_INITIALIZE,
    MCP_METHOD_INITIALIZED,
    MCP_METHOD_TOOLS_LIST,
    MCP_METHOD_TOOLS_CALL,
    MCP_METHOD_RESOURCES_LIST,
    MCP_METHOD_RESOURCES_READ,
    MCP_METHOD_PROMPTS_LIST,
    MCP_METHOD_PROMPTS_GET
} MCPMethod;

// MCP tool definition
typedef struct {
    char* name;
    char* description;
    String input_schema;  // JSON schema
} MCPToolDef;

// MCP resource definition
typedef struct {
    char* uri;
    char* name;
    char* description;
    char* mime_type;
} MCPResource;

// MCP prompt definition
typedef struct {
    char* name;
    char* description;
    String arguments;  // JSON array of argument definitions
} MCPPrompt;

// MCP client connection
typedef struct {
    char* server_id;
    char* transport_type;  // "stdio", "sse", "websocket"
    char* command;         // For stdio transport
    char** args;           // Command arguments
    size_t args_count;
    EnvVarArray env;       // Environment variables
    void* transport_data;  // Transport-specific data
    bool connected;
    MCPToolDef* tools;
    size_t tools_count;
    MCPResource* resources;
    size_t resources_count;
    MCPPrompt* prompts;
    size_t prompts_count;
} MCPClient;

// MCP manager
typedef struct {
    MCPClient** clients;
    size_t clients_count;
    size_t clients_capacity;
    char* workspace;
} MCPManager;

// MCP request/response structures
typedef struct {
    char* jsonrpc;
    long id;
    MCPMethod method;
    String params;  // JSON params
} MCPRequest;

typedef struct {
    char* jsonrpc;
    long id;
    String result;  // JSON result
    String error;   // JSON error
} MCPResponse;

// MCP Manager functions
MCPManager* mcp_manager_create(const char* workspace);
void mcp_manager_free(MCPManager* mgr);

// Client management
int mcp_manager_add_client(MCPManager* mgr, const char* server_id, const char* transport,
                           const char* command, char** args, size_t args_count,
                           EnvVar* env_vars, size_t env_count);
void mcp_manager_remove_client(MCPManager* mgr, const char* server_id);
MCPClient* mcp_manager_get_client(MCPManager* mgr, const char* server_id);

// Connection management
Error mcp_client_connect(MCPClient* client);
void mcp_client_disconnect(MCPClient* client);

// Tool management
Error mcp_client_list_tools(MCPClient* client, MCPToolDef** tools, size_t* count);
Error mcp_client_call_tool(MCPClient* client, const char* name, const char* params,
                           String* result);

// Resource management
Error mcp_client_list_resources(MCPClient* client, MCPResource** resources, size_t* count);
Error mcp_client_read_resource(MCPClient* client, const char* uri, String* content);

// Prompt management
Error mcp_client_list_prompts(MCPClient* client, MCPPrompt** prompts, size_t* count);
Error mcp_client_get_prompt(MCPClient* client, const char* name, const char* args,
                            String* result);

// JSON-RPC helpers
char* mcp_create_request(MCPMethod method, long id, const char* params_json);
char* mcp_parse_response(const char* json, MCPResponse* response);
void mcp_response_free(MCPResponse* response);

// Utility
const char* mcp_method_to_string(MCPMethod method);
MCPMethod mcp_method_from_string(const char* str);

// Tool registration (bridge to ToolRegistry)
void mcp_register_tools(ToolRegistry* reg, MCPClient* client);

#endif // MCP_H
