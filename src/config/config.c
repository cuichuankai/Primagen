#include "../include/config.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct Config {
    AgentConfig agent;
    ToolConfig tools;
    HeartbeatConfig heartbeat;
    ChannelsConfig channels;
};

Config* config_create() {
    Config* cfg = malloc(sizeof(Config));
    if (!cfg) return NULL;

    // Default agent config
    cfg->agent.model = strdup("gpt-4");
    cfg->agent.temperature = 0.1;
    cfg->agent.max_tokens = 4096;
    cfg->agent.max_tool_iterations = 40;
    cfg->agent.memory_window = 100;
    cfg->agent.reasoning_effort = strdup("medium");

    // Default tool config
    cfg->tools.exec.timeout = 300;
    cfg->tools.exec.restrict_to_workspace = false;
    cfg->tools.exec.path_append = strdup("");
    cfg->tools.web.search.enabled = true;
    cfg->tools.web.search.api_key = strdup("");
    cfg->tools.web.proxy = strdup("");
    cfg->tools.restrict_to_workspace = false;

    // Default heartbeat config
    cfg->heartbeat.enabled = true;
    cfg->heartbeat.interval_s = 300;

    // Default channels config
    cfg->channels.telegram.enabled = false;
    cfg->channels.telegram.token = strdup("");
    cfg->channels.telegram.allow_from = string_array_new();
    cfg->channels.whatsapp.enabled = false;
    cfg->channels.whatsapp.bridge_url = strdup("ws://localhost:3001");
    cfg->channels.whatsapp.bridge_token = strdup("");
    cfg->channels.whatsapp.allow_from = string_array_new();
    cfg->channels.send_progress = true;
    cfg->channels.send_tool_hints = true;

    return cfg;
}

void config_destroy(Config* cfg) {
    if (!cfg) return;

    free(cfg->agent.model);
    free(cfg->agent.reasoning_effort);
    free(cfg->tools.exec.path_append);
    free(cfg->tools.web.search.api_key);
    free(cfg->tools.web.proxy);
    free(cfg->channels.telegram.token);
    string_array_free(&cfg->channels.telegram.allow_from);
    free(cfg->channels.whatsapp.bridge_url);
    free(cfg->channels.whatsapp.bridge_token);
    string_array_free(&cfg->channels.whatsapp.allow_from);

    free(cfg);
}

AgentConfig* config_get_agent_config(Config* cfg) {
    return cfg ? &cfg->agent : NULL;
}

ToolConfig* config_get_tool_config(Config* cfg) {
    return cfg ? &cfg->tools : NULL;
}

HeartbeatConfig* config_get_heartbeat_config(Config* cfg) {
    return cfg ? &cfg->heartbeat : NULL;
}

ChannelsConfig* config_get_channels_config(Config* cfg) {
    return cfg ? &cfg->channels : NULL;
}

bool config_load_from_file(Config* cfg, const char* filepath) {
    // Stub implementation - would parse JSON config file
    (void)cfg;
    (void)filepath;
    return true;
}

bool config_save_to_file(Config* cfg, const char* filepath) {
    // Stub implementation - would write JSON config file
    (void)cfg;
    (void)filepath;
    return true;
}