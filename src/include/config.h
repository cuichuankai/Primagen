#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include "../vendor/cJSON/cJSON.h"
#include "common.h"

typedef struct {
    char* model;
    char* api_key;
    char* api_base;
    double temperature;
    int max_tokens;
    int max_tool_iterations;
    int memory_window;
    char* reasoning_effort;
    // Memory settings
    int memory_max_tokens;
    double memory_consolidation_threshold;
} AgentConfig;

typedef struct {
    bool enabled;
    int interval_s;
} HeartbeatConfig;

typedef struct {
    char* level;
    bool console_output;
} LogConfig;

typedef struct {
    bool enabled;
    char* api_key;
} WebSearchConfig;

typedef struct {
    int timeout;
    bool restrict_to_workspace;
    char* path_append;
} ExecToolConfig;

typedef struct {
    ExecToolConfig exec;
    bool restrict_to_workspace;
} ToolConfig;

typedef struct {
    char* dns4;
    char* dns6;
    int dns_timeout_ms;
} DNSConfig;

typedef struct {
    char* server_id;
    char* transport_type;  // "stdio", "sse", "websocket"
    char* command;
    StringArray args;
    EnvVarArray env;       // Environment variables for this server
    EnvVarArray headers;   // Custom HTTP headers for sse/streamable_http
} MCPServerConfig;

typedef struct {
    MCPServerConfig* servers;
    size_t server_count;
    size_t server_capacity;
    bool enabled;
} MCPConfig;

// Plugin configuration structures
typedef struct {
    char* plugin_id;      // Plugin unique identifier
    bool enabled;
    cJSON* config;        // Plugin configuration JSON object
} PluginConfig;

typedef struct {
    PluginConfig* items;  // Plugin configuration array
    size_t count;         // Current count
    size_t capacity;      // Capacity
} PluginsConfig;

typedef struct Config {
    AgentConfig agent;
    ToolConfig tools;
    DNSConfig dns;
    HeartbeatConfig heartbeat;
    MCPConfig mcp;
    LogConfig log;
    PluginsConfig plugins;  // New: plugin configuration
} Config;

Config* config_create();
void config_destroy(Config* cfg);

AgentConfig* config_get_agent_config(Config* cfg);
ToolConfig* config_get_tool_config(Config* cfg);
DNSConfig* config_get_dns_config(Config* cfg);
HeartbeatConfig* config_get_heartbeat_config(Config* cfg);
MCPConfig* config_get_mcp_config(Config* cfg);

bool config_load_from_file(Config* cfg, const char* filepath);
bool config_save_to_file(Config* cfg, const char* filepath);

// Environment variable overrides (env takes precedence over file)
void config_load_from_env(Config* cfg);

// Plugin configuration functions
PluginConfig* config_add_plugin_config(Config* cfg, const char* plugin_id);
PluginConfig* config_get_plugin_config(Config* cfg, const char* plugin_id);
void config_plugin_config_free(PluginConfig* item);

#endif // CONFIG_H
