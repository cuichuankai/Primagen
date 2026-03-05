#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
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
    char* imap_host;
    int imap_port;
    char* imap_username;
    char* imap_password;
    bool imap_use_ssl;
    char* smtp_host;
    int smtp_port;
    char* smtp_username;
    char* smtp_password;
    bool smtp_use_ssl;
    bool smtp_use_tls;
    char* from_address;
    StringArray allow_from;
} EmailChannelConfig;

typedef struct {
    bool enabled;
    char* token;
    char* gateway_url;
    int intents;
    StringArray allow_from;
} DiscordChannelConfig;

typedef struct {
    bool enabled;
    char* bot_token;
    char* app_token;
    char* mode;
    StringArray allow_from;
} SlackChannelConfig;

typedef struct {
    bool enabled;
    char* client_id;
    char* client_secret;
    StringArray allow_from;
} DingTalkChannelConfig;

typedef struct {
    bool enabled;
    char* app_id;
    char* app_secret;
    StringArray allow_from;
} FeishuChannelConfig;

typedef struct {
    bool enabled;
    char* bridge_url;
    char* bridge_token;
    StringArray allow_from;
} WhatsAppChannelConfig;

typedef struct {
    TelegramChannelConfig telegram;
    EmailChannelConfig email;
    DiscordChannelConfig discord;
    SlackChannelConfig slack;
    DingTalkChannelConfig dingtalk;
    FeishuChannelConfig feishu;
    WhatsAppChannelConfig whatsapp;
    bool send_progress;
    bool send_tool_hints;
} ChannelsConfig;

typedef struct Config {
    AgentConfig agent;
    ToolConfig tools;
    HeartbeatConfig heartbeat;
    ChannelsConfig channels;
} Config;

Config* config_create();
void config_destroy(Config* cfg);

AgentConfig* config_get_agent_config(Config* cfg);
ToolConfig* config_get_tool_config(Config* cfg);
HeartbeatConfig* config_get_heartbeat_config(Config* cfg);
ChannelsConfig* config_get_channels_config(Config* cfg);

bool config_load_from_file(Config* cfg, const char* filepath);
bool config_save_to_file(Config* cfg, const char* filepath);

#endif // CONFIG_H
