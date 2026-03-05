#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include "common.h"

typedef struct Config Config;

typedef struct {
    char* model;
    double temperature;
    int max_tokens;
    int max_tool_iterations;
    int memory_window;
    char* reasoning_effort;
} AgentConfig;

typedef struct {
    bool enabled;
    int interval_s;
} HeartbeatConfig;

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
    WebSearchConfig search;
    char* proxy;
} WebToolConfig;

typedef struct {
    ExecToolConfig exec;
    WebToolConfig web;
    bool restrict_to_workspace;
} ToolConfig;

typedef struct {
    bool enabled;
    char* token;
    StringArray allow_from;
} TelegramChannelConfig;

typedef struct {
    bool enabled;
    char* bridge_url;
    char* bridge_token;
    StringArray allow_from;
} WhatsAppChannelConfig;

typedef struct {
    TelegramChannelConfig telegram;
    WhatsAppChannelConfig whatsapp;
    bool send_progress;
    bool send_tool_hints;
} ChannelsConfig;

Config* config_create();
void config_destroy(Config* cfg);

AgentConfig* config_get_agent_config(Config* cfg);
ToolConfig* config_get_tool_config(Config* cfg);
HeartbeatConfig* config_get_heartbeat_config(Config* cfg);
ChannelsConfig* config_get_channels_config(Config* cfg);

bool config_load_from_file(Config* cfg, const char* filepath);
bool config_save_to_file(Config* cfg, const char* filepath);

#endif // CONFIG_H