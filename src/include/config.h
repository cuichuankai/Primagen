#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include "../vendor/cJSON/cJSON.h"
#include "common.h"

typedef struct {
    char* provider;
    int max_tool_iterations;
    int memory_window;
    int memory_max_tokens;
    double memory_consolidation_threshold;
} AgentConfig;

typedef struct {
    char* name;
    char* model;
    char* api_key;
    char* api_base;
    double temperature;
    int max_tokens;
    char* reasoning_effort;
} ProviderConfig;

typedef struct {
    ProviderConfig* items;
    size_t count;
    size_t capacity;
} ProvidersConfig;

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
    char* dns4_url;
    char* dns6_url;
    int dns_timeout_ms;
    bool use_system_resolver;
} DNSConfig;

typedef struct {
    char* plugin_id;
    bool enabled;
    cJSON* config;
} PluginConfig;

typedef struct {
    PluginConfig* items;
    size_t count;
    size_t capacity;
} PluginsConfig;

typedef struct Config {
    AgentConfig agent;
    ProvidersConfig providers;
    ToolConfig tools;
    DNSConfig dns;
    HeartbeatConfig heartbeat;
    LogConfig log;
    PluginsConfig plugins;
} Config;

Config* config_create();
void config_destroy(Config* cfg);

AgentConfig* config_get_agent_config(Config* cfg);
ToolConfig* config_get_tool_config(Config* cfg);
DNSConfig* config_get_dns_config(Config* cfg);
HeartbeatConfig* config_get_heartbeat_config(Config* cfg);

ProviderConfig* config_add_provider(Config* cfg, const char* name);
ProviderConfig* config_get_provider(Config* cfg, const char* name);
ProviderConfig* config_get_active_provider(Config* cfg);
void config_provider_config_free(ProviderConfig* item);

bool config_load_from_file(Config* cfg, const char* filepath);
bool config_save_to_file(Config* cfg, const char* filepath);
void config_load_from_env(Config* cfg);

PluginConfig* config_add_plugin_config(Config* cfg, const char* plugin_id);
PluginConfig* config_get_plugin_config(Config* cfg, const char* plugin_id);
void config_plugin_config_free(PluginConfig* item);

Error config_validate(const Config* cfg);

#endif // CONFIG_H