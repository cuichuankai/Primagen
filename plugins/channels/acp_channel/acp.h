#ifndef ACP_H
#define ACP_H

#include "../../../src/include/common.h"
#include "../../../src/bus/message_bus.h"
#include "../../../src/tools/tool.h"
#include "../../../src/agent/agent_loop.h"
#include "../../../src/session/session.h"
#include "../../../src/include/config.h"

struct mg_connection;
struct mg_http_message;

// ACP Server configuration
#define ACP_DEFAULT_PORT 8080
#define ACP_DEFAULT_HOST "127.0.0.1"

// ACP API routes
#define ACP_ROUTE_TOOLS_LIST "/v1/tools/list"
#define ACP_ROUTE_TOOLS_CALL "/v1/tools/call"
#define ACP_ROUTE_CHAT_COMPLETIONS "/v1/chat/completions"
#define ACP_ROUTE_CHAT_RESPONSES "/v1/chat/responses"
#define ACP_ROUTE_HEALTH "/v1/health"

// Max concurrent connections
#define ACP_MAX_CONNECTIONS 100

// ACP Server state
typedef struct {
    int port;
    char* host;
    bool running;

    // References to Primagen components
    MessageBus* bus;
    ToolRegistry* tool_registry;
    AgentLoop* agent_loop;
    SessionManager* session_mgr;
    Config* config;

    // Mongoose connection manager
    struct mg_mgr* mgr;
    struct mg_connection* nc;

    // Connection tracking
    int active_connections;
    pthread_mutex_t connection_mutex;
} ACPServer;

// OpenAI-compatible chat completion request
typedef struct {
    char* model;
    String messages;  // JSON array of {role, content}
    float temperature;
    int max_tokens;
    bool stream;
    char* session_id;
} ChatCompletionRequest;

// OpenAI-compatible chat completion response
typedef struct {
    char* id;
    char* object;
    long created;
    char* model;
    String choices;  // JSON array of {index, message, finish_reason}
    String usage;    // JSON object {prompt_tokens, completion_tokens, total_tokens}
} ChatCompletionResponse;

// Tool call request/response
typedef struct {
    char* tool_name;
    String arguments;  // JSON object
    char* session_id;
} ToolCallRequest;

typedef struct {
    char* tool_name;
    String result;     // JSON result
    char* error;       // Error message if failed
} ToolCallResponse;

// ACPServer lifecycle
ACPServer* acp_server_new(
    MessageBus* bus,
    ToolRegistry* tool_registry,
    AgentLoop* agent_loop,
    SessionManager* session_mgr,
    Config* config
);

void acp_server_free(ACPServer* server);

int acp_server_start(ACPServer* server, int port, const char* host);
void acp_server_stop(ACPServer* server);

// HTTP request handlers
void acp_handle_tools_list(struct mg_connection* nc, ACPServer* server);
void acp_handle_tools_call(struct mg_connection* nc, ACPServer* server, const char* body);
void acp_handle_chat_completions(struct mg_connection* nc, ACPServer* server, const char* body);
void acp_handle_chat_responses(struct mg_connection* nc, ACPServer* server, const struct mg_http_message* hm);
void acp_handle_health(struct mg_connection* nc, ACPServer* server);

// JSON helpers
char* acp_tools_list_json(ToolRegistry* registry);
char* acp_tool_response_json(const char* tool_name, const char* result, const char* error);
char* acp_chat_response_json(const char* id, const char* model, const char* content, const char* finish_reason);
char* acp_error_json(int code, const char* message);

// Event handler
void acp_event_handler(struct mg_connection* nc, int ev, void* ev_data);

#endif // ACP_H
