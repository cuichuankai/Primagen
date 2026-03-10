#include "../include/config.h"
#include "../vendor/cJSON/cJSON.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Struct definition moved to header

// Helper functions for cJSON
static char* get_json_string(cJSON* item, const char* default_val) {
    if (cJSON_IsString(item) && (item->valuestring != NULL)) {
        return strdup(item->valuestring);
    }
    return strdup(default_val);
}

static int get_json_int(cJSON* item, int default_val) {
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return default_val;
}

static double get_json_double(cJSON* item, double default_val) {
    if (cJSON_IsNumber(item)) {
        return item->valuedouble;
    }
    return default_val;
}

static bool get_json_bool(cJSON* item, bool default_val) {
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return default_val;
}

static void load_string_array(cJSON* array_item, StringArray* target) {
    if (!cJSON_IsArray(array_item)) return;
    
    string_array_free(target);
    *target = string_array_new();
    
    cJSON* item = NULL;
    cJSON_ArrayForEach(item, array_item) {
        if (cJSON_IsString(item)) {
            string_array_add(target, item->valuestring);
        }
    }
}

Config* config_create() {
    Config* cfg = malloc(sizeof(Config));
    if (!cfg) return NULL;

    // Default agent config
    cfg->agent.model = strdup("gpt-4");
    cfg->agent.api_key = strdup("");
    cfg->agent.api_base = strdup("https://api.openai.com/v1");
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

    // Default log config
    cfg->log.level = strdup("INFO");
    cfg->log.console_output = false;

    // Default channels config
    cfg->channels.telegram.enabled = false;
    cfg->channels.telegram.token = strdup("");
    cfg->channels.telegram.allow_from = string_array_new();

    cfg->channels.email.enabled = false;
    cfg->channels.email.imap_host = strdup("");
    cfg->channels.email.imap_port = 993;
    cfg->channels.email.imap_username = strdup("");
    cfg->channels.email.imap_password = strdup("");
    cfg->channels.email.imap_use_ssl = true;
    cfg->channels.email.smtp_host = strdup("");
    cfg->channels.email.smtp_port = 465;
    cfg->channels.email.smtp_username = strdup("");
    cfg->channels.email.smtp_password = strdup("");
    cfg->channels.email.smtp_use_ssl = true;
    cfg->channels.email.smtp_use_tls = false;
    cfg->channels.email.from_address = strdup("");
    cfg->channels.email.allow_from = string_array_new();

    cfg->channels.discord.enabled = false;
    cfg->channels.discord.token = strdup("");
    cfg->channels.discord.gateway_url = strdup("wss://gateway.discord.gg");
    cfg->channels.discord.intents = 33280; // Default intents
    cfg->channels.discord.allow_from = string_array_new();

    cfg->channels.slack.enabled = false;
    cfg->channels.slack.bot_token = strdup("");
    cfg->channels.slack.app_token = strdup("");
    cfg->channels.slack.mode = strdup("socket");
    cfg->channels.slack.allow_from = string_array_new();

    cfg->channels.dingtalk.enabled = false;
    cfg->channels.dingtalk.client_id = strdup("");
    cfg->channels.dingtalk.client_secret = strdup("");
    cfg->channels.dingtalk.allow_from = string_array_new();

    cfg->channels.feishu.enabled = false;
    cfg->channels.feishu.app_id = strdup("");
    cfg->channels.feishu.app_secret = strdup("");
    cfg->channels.feishu.use_card = false;
    cfg->channels.feishu.allow_from = string_array_new();

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
    free(cfg->agent.api_key);
    free(cfg->agent.api_base);
    free(cfg->agent.reasoning_effort);
    free(cfg->tools.exec.path_append);
    free(cfg->tools.web.search.api_key);
    free(cfg->tools.web.proxy);
    
    free(cfg->log.level);
    
    free(cfg->channels.telegram.token);
    string_array_free(&cfg->channels.telegram.allow_from);

    free(cfg->channels.email.imap_host);
    free(cfg->channels.email.imap_username);
    free(cfg->channels.email.imap_password);
    free(cfg->channels.email.smtp_host);
    free(cfg->channels.email.smtp_username);
    free(cfg->channels.email.smtp_password);
    free(cfg->channels.email.from_address);
    string_array_free(&cfg->channels.email.allow_from);

    free(cfg->channels.discord.token);
    free(cfg->channels.discord.gateway_url);
    string_array_free(&cfg->channels.discord.allow_from);

    free(cfg->channels.slack.bot_token);
    free(cfg->channels.slack.app_token);
    free(cfg->channels.slack.mode);
    string_array_free(&cfg->channels.slack.allow_from);

    free(cfg->channels.dingtalk.client_id);
    free(cfg->channels.dingtalk.client_secret);
    string_array_free(&cfg->channels.dingtalk.allow_from);

    free(cfg->channels.feishu.app_id);
    free(cfg->channels.feishu.app_secret);
    string_array_free(&cfg->channels.feishu.allow_from);

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
    FILE* fp = fopen(filepath, "r");
    if (!fp) {
        // Not an error if config file doesn't exist, we just use defaults
        return true; 
    }

    fseek(fp, 0, SEEK_END);
    long length = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    char* data = malloc(length + 1);
    if (!data) {
        fclose(fp);
        return false;
    }
    
    if (fread(data, 1, length, fp) != (size_t)length) {
        free(data);
        fclose(fp);
        return false;
    }
    data[length] = '\0';
    fclose(fp);
    
    cJSON* json = cJSON_Parse(data);
    free(data);
    
    if (!json) {
        return false;
    }
    
    // Agent Config
    cJSON* agent = cJSON_GetObjectItem(json, "agent");
    if (agent) {
        cJSON* item;
        if ((item = cJSON_GetObjectItem(agent, "model"))) {
            free(cfg->agent.model);
            cfg->agent.model = get_json_string(item, "gpt-4");
        }
        if ((item = cJSON_GetObjectItem(agent, "apiKey"))) {
            free(cfg->agent.api_key);
            cfg->agent.api_key = get_json_string(item, "");
        }
        if ((item = cJSON_GetObjectItem(agent, "apiBase"))) {
            free(cfg->agent.api_base);
            cfg->agent.api_base = get_json_string(item, "");
        }
        if ((item = cJSON_GetObjectItem(agent, "temperature"))) cfg->agent.temperature = get_json_double(item, 0.1);
        if ((item = cJSON_GetObjectItem(agent, "max_tokens"))) cfg->agent.max_tokens = get_json_int(item, 4096);
        if ((item = cJSON_GetObjectItem(agent, "max_tool_iterations"))) cfg->agent.max_tool_iterations = get_json_int(item, 40);
        if ((item = cJSON_GetObjectItem(agent, "memory_window"))) cfg->agent.memory_window = get_json_int(item, 100);
        if ((item = cJSON_GetObjectItem(agent, "reasoning_effort"))) {
            free(cfg->agent.reasoning_effort);
            cfg->agent.reasoning_effort = get_json_string(item, "medium");
        }
    }
    
    // Tools Config
    cJSON* tools = cJSON_GetObjectItem(json, "tools");
    if (tools) {
        cJSON* item;
        if ((item = cJSON_GetObjectItem(tools, "restrictToWorkspace"))) cfg->tools.restrict_to_workspace = get_json_bool(item, false);
        
        cJSON* exec = cJSON_GetObjectItem(tools, "exec");
        if (exec) {
            if ((item = cJSON_GetObjectItem(exec, "timeout"))) cfg->tools.exec.timeout = get_json_int(item, 300);
            if ((item = cJSON_GetObjectItem(exec, "restrictToWorkspace"))) cfg->tools.exec.restrict_to_workspace = get_json_bool(item, false);
            if ((item = cJSON_GetObjectItem(exec, "pathAppend"))) {
                free(cfg->tools.exec.path_append);
                cfg->tools.exec.path_append = get_json_string(item, "");
            }
        }
        
        cJSON* web = cJSON_GetObjectItem(tools, "web");
        if (web) {
            cJSON* search = cJSON_GetObjectItem(web, "search");
            if (search) {
                if ((item = cJSON_GetObjectItem(search, "enabled"))) cfg->tools.web.search.enabled = get_json_bool(item, true);
                if ((item = cJSON_GetObjectItem(search, "apiKey"))) {
                    free(cfg->tools.web.search.api_key);
                    cfg->tools.web.search.api_key = get_json_string(item, "");
                }
            }
            if ((item = cJSON_GetObjectItem(web, "proxy"))) {
                free(cfg->tools.web.proxy);
                cfg->tools.web.proxy = get_json_string(item, "");
            }
        }
    }
    
    // Heartbeat Config
    cJSON* heartbeat = cJSON_GetObjectItem(json, "heartbeat");
    if (heartbeat) {
        cJSON* item;
        if ((item = cJSON_GetObjectItem(heartbeat, "enabled"))) cfg->heartbeat.enabled = get_json_bool(item, true);
        if ((item = cJSON_GetObjectItem(heartbeat, "interval_s"))) cfg->heartbeat.interval_s = get_json_int(item, 300);
    }
    
    // Log Config
    cJSON* log = cJSON_GetObjectItem(json, "log");
    if (log) {
        cJSON* item;
        if ((item = cJSON_GetObjectItem(log, "level"))) {
            free(cfg->log.level);
            cfg->log.level = get_json_string(item, "INFO");
        }
        if ((item = cJSON_GetObjectItem(log, "consoleOutput"))) cfg->log.console_output = get_json_bool(item, true);
    }

    // Channels Config
    cJSON* channels = cJSON_GetObjectItem(json, "channels");
    if (channels) {
        cJSON* item;
        if ((item = cJSON_GetObjectItem(channels, "sendProgress"))) cfg->channels.send_progress = get_json_bool(item, true);
        if ((item = cJSON_GetObjectItem(channels, "sendToolHints"))) cfg->channels.send_tool_hints = get_json_bool(item, true);
        
        cJSON* telegram = cJSON_GetObjectItem(channels, "telegram");
        if (telegram) {
            if ((item = cJSON_GetObjectItem(telegram, "enabled"))) cfg->channels.telegram.enabled = get_json_bool(item, false);
            if ((item = cJSON_GetObjectItem(telegram, "token"))) {
                free(cfg->channels.telegram.token);
                cfg->channels.telegram.token = get_json_string(item, "");
            }
            load_string_array(cJSON_GetObjectItem(telegram, "allowFrom"), &cfg->channels.telegram.allow_from);
        }

        cJSON* email = cJSON_GetObjectItem(channels, "email");
        if (email) {
            if ((item = cJSON_GetObjectItem(email, "enabled"))) cfg->channels.email.enabled = get_json_bool(item, false);
            if ((item = cJSON_GetObjectItem(email, "imapHost"))) { free(cfg->channels.email.imap_host); cfg->channels.email.imap_host = get_json_string(item, ""); }
            if ((item = cJSON_GetObjectItem(email, "imapPort"))) cfg->channels.email.imap_port = get_json_int(item, 993);
            if ((item = cJSON_GetObjectItem(email, "imapUsername"))) { free(cfg->channels.email.imap_username); cfg->channels.email.imap_username = get_json_string(item, ""); }
            if ((item = cJSON_GetObjectItem(email, "imapPassword"))) { free(cfg->channels.email.imap_password); cfg->channels.email.imap_password = get_json_string(item, ""); }
            if ((item = cJSON_GetObjectItem(email, "imapUseSsl"))) cfg->channels.email.imap_use_ssl = get_json_bool(item, true);
            if ((item = cJSON_GetObjectItem(email, "smtpHost"))) { free(cfg->channels.email.smtp_host); cfg->channels.email.smtp_host = get_json_string(item, ""); }
            if ((item = cJSON_GetObjectItem(email, "smtpPort"))) cfg->channels.email.smtp_port = get_json_int(item, 465);
            if ((item = cJSON_GetObjectItem(email, "smtpUsername"))) { free(cfg->channels.email.smtp_username); cfg->channels.email.smtp_username = get_json_string(item, ""); }
            if ((item = cJSON_GetObjectItem(email, "smtpPassword"))) { free(cfg->channels.email.smtp_password); cfg->channels.email.smtp_password = get_json_string(item, ""); }
            if ((item = cJSON_GetObjectItem(email, "smtpUseSsl"))) cfg->channels.email.smtp_use_ssl = get_json_bool(item, true);
            if ((item = cJSON_GetObjectItem(email, "smtpUseTls"))) cfg->channels.email.smtp_use_tls = get_json_bool(item, false);
            if ((item = cJSON_GetObjectItem(email, "fromAddress"))) { free(cfg->channels.email.from_address); cfg->channels.email.from_address = get_json_string(item, ""); }
            load_string_array(cJSON_GetObjectItem(email, "allowFrom"), &cfg->channels.email.allow_from);
        }

        cJSON* discord = cJSON_GetObjectItem(channels, "discord");
        if (discord) {
            if ((item = cJSON_GetObjectItem(discord, "enabled"))) cfg->channels.discord.enabled = get_json_bool(item, false);
            if ((item = cJSON_GetObjectItem(discord, "token"))) { free(cfg->channels.discord.token); cfg->channels.discord.token = get_json_string(item, ""); }
            if ((item = cJSON_GetObjectItem(discord, "gatewayUrl"))) { free(cfg->channels.discord.gateway_url); cfg->channels.discord.gateway_url = get_json_string(item, "wss://gateway.discord.gg"); }
            if ((item = cJSON_GetObjectItem(discord, "intents"))) cfg->channels.discord.intents = get_json_int(item, 33280);
            load_string_array(cJSON_GetObjectItem(discord, "allowFrom"), &cfg->channels.discord.allow_from);
        }

        cJSON* slack = cJSON_GetObjectItem(channels, "slack");
        if (slack) {
            if ((item = cJSON_GetObjectItem(slack, "enabled"))) cfg->channels.slack.enabled = get_json_bool(item, false);
            if ((item = cJSON_GetObjectItem(slack, "botToken"))) { free(cfg->channels.slack.bot_token); cfg->channels.slack.bot_token = get_json_string(item, ""); }
            if ((item = cJSON_GetObjectItem(slack, "appToken"))) { free(cfg->channels.slack.app_token); cfg->channels.slack.app_token = get_json_string(item, ""); }
            if ((item = cJSON_GetObjectItem(slack, "mode"))) { free(cfg->channels.slack.mode); cfg->channels.slack.mode = get_json_string(item, "socket"); }
            load_string_array(cJSON_GetObjectItem(slack, "allowFrom"), &cfg->channels.slack.allow_from);
        }

        cJSON* dingtalk = cJSON_GetObjectItem(channels, "dingtalk");
        if (dingtalk) {
            if ((item = cJSON_GetObjectItem(dingtalk, "enabled"))) cfg->channels.dingtalk.enabled = get_json_bool(item, false);
            if ((item = cJSON_GetObjectItem(dingtalk, "clientId"))) { free(cfg->channels.dingtalk.client_id); cfg->channels.dingtalk.client_id = get_json_string(item, ""); }
            if ((item = cJSON_GetObjectItem(dingtalk, "clientSecret"))) { free(cfg->channels.dingtalk.client_secret); cfg->channels.dingtalk.client_secret = get_json_string(item, ""); }
            load_string_array(cJSON_GetObjectItem(dingtalk, "allowFrom"), &cfg->channels.dingtalk.allow_from);
        }

        cJSON* feishu = cJSON_GetObjectItem(channels, "feishu");
        if (feishu) {
            if ((item = cJSON_GetObjectItem(feishu, "enabled"))) cfg->channels.feishu.enabled = get_json_bool(item, false);
            if ((item = cJSON_GetObjectItem(feishu, "app_id"))) { free(cfg->channels.feishu.app_id); cfg->channels.feishu.app_id = get_json_string(item, ""); }
            if ((item = cJSON_GetObjectItem(feishu, "app_secret"))) { free(cfg->channels.feishu.app_secret); cfg->channels.feishu.app_secret = get_json_string(item, ""); }
            if ((item = cJSON_GetObjectItem(feishu, "useCard"))) cfg->channels.feishu.use_card = get_json_bool(item, false);
            load_string_array(cJSON_GetObjectItem(feishu, "allow_from"), &cfg->channels.feishu.allow_from);
        }
        
        cJSON* whatsapp = cJSON_GetObjectItem(channels, "whatsapp");
        if (whatsapp) {
            if ((item = cJSON_GetObjectItem(whatsapp, "enabled"))) cfg->channels.whatsapp.enabled = get_json_bool(item, false);
            if ((item = cJSON_GetObjectItem(whatsapp, "bridgeUrl"))) {
                free(cfg->channels.whatsapp.bridge_url);
                cfg->channels.whatsapp.bridge_url = get_json_string(item, "ws://localhost:3001");
            }
            if ((item = cJSON_GetObjectItem(whatsapp, "bridgeToken"))) {
                free(cfg->channels.whatsapp.bridge_token);
                cfg->channels.whatsapp.bridge_token = get_json_string(item, "");
            }
            load_string_array(cJSON_GetObjectItem(whatsapp, "allowFrom"), &cfg->channels.whatsapp.allow_from);
        }
    }
    
    cJSON_Delete(json);
    return true;
}

bool config_save_to_file(Config* cfg, const char* filepath) {
    cJSON* json = cJSON_CreateObject();
    
    // Agent
    cJSON* agent = cJSON_CreateObject();
    cJSON_AddStringToObject(agent, "model", cfg->agent.model);
    cJSON_AddStringToObject(agent, "apiKey", cfg->agent.api_key);
    cJSON_AddStringToObject(agent, "apiBase", cfg->agent.api_base);
    cJSON_AddNumberToObject(agent, "temperature", cfg->agent.temperature);
    cJSON_AddNumberToObject(agent, "max_tokens", cfg->agent.max_tokens);
    cJSON_AddNumberToObject(agent, "max_tool_iterations", cfg->agent.max_tool_iterations);
    cJSON_AddNumberToObject(agent, "memory_window", cfg->agent.memory_window);
    cJSON_AddStringToObject(agent, "reasoning_effort", cfg->agent.reasoning_effort);
    cJSON_AddItemToObject(json, "agent", agent);
    
    // Tools
    cJSON* tools = cJSON_CreateObject();
    cJSON_AddBoolToObject(tools, "restrictToWorkspace", cfg->tools.restrict_to_workspace);
    
    cJSON* exec = cJSON_CreateObject();
    cJSON_AddNumberToObject(exec, "timeout", cfg->tools.exec.timeout);
    cJSON_AddBoolToObject(exec, "restrictToWorkspace", cfg->tools.exec.restrict_to_workspace);
    cJSON_AddStringToObject(exec, "pathAppend", cfg->tools.exec.path_append);
    cJSON_AddItemToObject(tools, "exec", exec);
    
    cJSON* web = cJSON_CreateObject();
    cJSON* search = cJSON_CreateObject();
    cJSON_AddBoolToObject(search, "enabled", cfg->tools.web.search.enabled);
    cJSON_AddStringToObject(search, "apiKey", cfg->tools.web.search.api_key);
    cJSON_AddItemToObject(web, "search", search);
    cJSON_AddStringToObject(web, "proxy", cfg->tools.web.proxy);
    cJSON_AddItemToObject(tools, "web", web);
    
    cJSON_AddItemToObject(json, "tools", tools);
    
    // Heartbeat
    cJSON* heartbeat = cJSON_CreateObject();
    cJSON_AddBoolToObject(heartbeat, "enabled", cfg->heartbeat.enabled);
    cJSON_AddNumberToObject(heartbeat, "interval_s", cfg->heartbeat.interval_s);
    cJSON_AddItemToObject(json, "heartbeat", heartbeat);
    
    // Log
    cJSON* log = cJSON_CreateObject();
    cJSON_AddStringToObject(log, "level", cfg->log.level);
    cJSON_AddBoolToObject(log, "consoleOutput", cfg->log.console_output);
    cJSON_AddItemToObject(json, "log", log);

    // Channels
    cJSON* channels = cJSON_CreateObject();
    cJSON_AddBoolToObject(channels, "sendProgress", cfg->channels.send_progress);
    cJSON_AddBoolToObject(channels, "sendToolHints", cfg->channels.send_tool_hints);
    
    cJSON* telegram = cJSON_CreateObject();
    cJSON_AddBoolToObject(telegram, "enabled", cfg->channels.telegram.enabled);
    cJSON_AddStringToObject(telegram, "token", cfg->channels.telegram.token);
    // TODO: save allowFrom
    cJSON_AddItemToObject(channels, "telegram", telegram);
    
    cJSON* whatsapp = cJSON_CreateObject();
    cJSON_AddBoolToObject(whatsapp, "enabled", cfg->channels.whatsapp.enabled);
    cJSON_AddStringToObject(whatsapp, "bridgeUrl", cfg->channels.whatsapp.bridge_url);
    cJSON_AddStringToObject(whatsapp, "bridgeToken", cfg->channels.whatsapp.bridge_token);
    // TODO: save allowFrom
    cJSON_AddItemToObject(channels, "whatsapp", whatsapp);
    
    cJSON_AddItemToObject(json, "channels", channels);
    
    char* string = cJSON_Print(json);
    cJSON_Delete(json);
    
    if (!string) return false;
    
    FILE* fp = fopen(filepath, "w");
    if (!fp) {
        free(string);
        return false;
    }
    
    fputs(string, fp);
    fclose(fp);
    free(string);
    return true;
}
