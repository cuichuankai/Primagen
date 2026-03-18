#include "../include/config.h"
#include "../include/logger.h"
#include "../vendor/cJSON/cJSON.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Struct definition moved to header

// Helper functions for environment variables
static const char* get_env_string(const char* key, const char* default_val) {
    const char* val = getenv(key);
    return val ? val : default_val;
}

static int get_env_int(const char* key, int default_val) {
    const char* val = getenv(key);
    if (!val) return default_val;
    return atoi(val);
}

static double get_env_double(const char* key, double default_val) {
    const char* val = getenv(key);
    if (!val) return default_val;
    return atof(val);
}

static bool get_env_bool(const char* key, bool default_val) {
    const char* val = getenv(key);
    if (!val) return default_val;
    // Support common boolean formats: true/false, yes/no, 1/0
    return (strcmp(val, "true") == 0 || strcmp(val, "yes") == 0 || strcmp(val, "1") == 0);
}

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

// PluginConfig helper functions
PluginConfig* config_add_plugin_config(Config* cfg, const char* plugin_id) {
    if (!cfg || !plugin_id) return NULL;

    // Check if plugin already exists
    for (size_t i = 0; i < cfg->plugins.count; i++) {
        if (cfg->plugins.items[i].plugin_id &&
            strcmp(cfg->plugins.items[i].plugin_id, plugin_id) == 0) {
            return &cfg->plugins.items[i];
        }
    }

    // Grow array if needed
    if (cfg->plugins.count >= cfg->plugins.capacity) {
        size_t new_capacity = cfg->plugins.capacity == 0 ? 4 : cfg->plugins.capacity * 2;
        PluginConfig* new_items = realloc(cfg->plugins.items, new_capacity * sizeof(PluginConfig));
        if (!new_items) return NULL;
        cfg->plugins.items = new_items;
        cfg->plugins.capacity = new_capacity;
    }

    // Initialize new item
    PluginConfig* item = &cfg->plugins.items[cfg->plugins.count];
    item->plugin_id = strdup(plugin_id);
    item->config = NULL;
    cfg->plugins.count++;

    return item;
}

PluginConfig* config_get_plugin_config(Config* cfg, const char* plugin_id) {
    if (!cfg || !plugin_id) return NULL;

    for (size_t i = 0; i < cfg->plugins.count; i++) {
        if (cfg->plugins.items[i].plugin_id &&
            strcmp(cfg->plugins.items[i].plugin_id, plugin_id) == 0) {
            return &cfg->plugins.items[i];
        }
    }

    return NULL;
}

void config_plugin_config_free(PluginConfig* item) {
    if (!item) return;
    free(item->plugin_id);
    if (item->config) {
        cJSON_Delete(item->config);
    }
    item->plugin_id = NULL;
    item->config = NULL;
}

// EnvVarArray helper functions
static EnvVarArray env_var_array_new() {
    EnvVarArray arr;
    arr.items = NULL;
    arr.count = 0;
    arr.capacity = 0;
    return arr;
}

static void env_var_array_add(EnvVarArray* arr, const char* key, const char* value) {
    if (!arr) return;
    if (arr->count >= arr->capacity) {
        size_t new_capacity = arr->capacity == 0 ? 4 : arr->capacity * 2;
        EnvVar* new_items = realloc(arr->items, new_capacity * sizeof(EnvVar));
        if (new_items) {
            arr->items = new_items;
            arr->capacity = new_capacity;
        } else {
            return;
        }
    }
    arr->items[arr->count].key = strdup(key);
    arr->items[arr->count].value = strdup(value);
    arr->count++;
}

static void env_var_array_free(EnvVarArray* arr) {
    if (!arr) return;
    for (size_t i = 0; i < arr->count; i++) {
        free(arr->items[i].key);
        free(arr->items[i].value);
    }
    free(arr->items);
    arr->items = NULL;
    arr->count = 0;
    arr->capacity = 0;
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

    cfg->channels.whatsapp.enabled = false;
    cfg->channels.whatsapp.bridge_url = strdup("ws://localhost:3001");
    cfg->channels.whatsapp.bridge_token = strdup("");
    cfg->channels.whatsapp.allow_from = string_array_new();
    cfg->channels.send_progress = true;
    cfg->channels.send_tool_hints = true;

    // Default MCP config
    cfg->mcp.enabled = false;
    cfg->mcp.servers = NULL;
    cfg->mcp.server_count = 0;
    cfg->mcp.server_capacity = 0;

    // Default plugins config
    cfg->plugins.items = NULL;
    cfg->plugins.count = 0;
    cfg->plugins.capacity = 0;

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

    free(cfg->channels.whatsapp.bridge_url);
    free(cfg->channels.whatsapp.bridge_token);
    string_array_free(&cfg->channels.whatsapp.allow_from);

    // Free MCP config
    for (size_t i = 0; i < cfg->mcp.server_count; i++) {
        free(cfg->mcp.servers[i].server_id);
        free(cfg->mcp.servers[i].transport_type);
        free(cfg->mcp.servers[i].command);
        string_array_free(&cfg->mcp.servers[i].args);
        env_var_array_free(&cfg->mcp.servers[i].env);
    }
    free(cfg->mcp.servers);

    // Free plugins config
    for (size_t i = 0; i < cfg->plugins.count; i++) {
        config_plugin_config_free(&cfg->plugins.items[i]);
    }
    free(cfg->plugins.items);

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

MCPConfig* config_get_mcp_config(Config* cfg) {
    return cfg ? &cfg->mcp : NULL;
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

    // MCP Config
    cJSON* mcp = cJSON_GetObjectItem(json, "mcp");
    if (mcp) {
        cJSON* item;
        if ((item = cJSON_GetObjectItem(mcp, "enabled"))) {
            cfg->mcp.enabled = get_json_bool(item, false);
        }

        cJSON* servers = cJSON_GetObjectItem(mcp, "servers");
        if (cJSON_IsArray(servers)) {
            cJSON* server_item;
            cJSON_ArrayForEach(server_item, servers) {
                // Grow array if needed
                if (cfg->mcp.server_count >= cfg->mcp.server_capacity) {
                    size_t new_capacity = cfg->mcp.server_capacity == 0 ? 4 : cfg->mcp.server_capacity * 2;
                    MCPServerConfig* new_servers = realloc(cfg->mcp.servers, new_capacity * sizeof(MCPServerConfig));
                    if (new_servers) {
                        cfg->mcp.servers = new_servers;
                        cfg->mcp.server_capacity = new_capacity;
                    } else {
                        break;
                    }
                }

                // Initialize new server
                MCPServerConfig* server = &cfg->mcp.servers[cfg->mcp.server_count];
                memset(server, 0, sizeof(MCPServerConfig));
                server->args = string_array_new();
                server->env = env_var_array_new();

                cJSON* s_item;
                if ((s_item = cJSON_GetObjectItem(server_item, "id"))) {
                    server->server_id = get_json_string(s_item, "");
                }
                if ((s_item = cJSON_GetObjectItem(server_item, "transport"))) {
                    server->transport_type = get_json_string(s_item, "stdio");
                }
                if ((s_item = cJSON_GetObjectItem(server_item, "command"))) {
                    server->command = get_json_string(s_item, "");
                }

                cJSON* args = cJSON_GetObjectItem(server_item, "args");
                if (cJSON_IsArray(args)) {
                    cJSON* arg_item;
                    cJSON_ArrayForEach(arg_item, args) {
                        if (cJSON_IsString(arg_item)) {
                            string_array_add(&server->args, arg_item->valuestring);
                        }
                    }
                }

                // Parse environment variables
                cJSON* env = cJSON_GetObjectItem(server_item, "env");
                if (cJSON_IsObject(env)) {
                    cJSON* env_item;
                    cJSON_ArrayForEach(env_item, env) {
                        if (cJSON_IsString(env_item)) {
                            env_var_array_add(&server->env, env_item->string, env_item->valuestring);
                        }
                    }
                }

                cfg->mcp.server_count++;
            }
        }
    }

    // Plugins Config
    cJSON* plugins = cJSON_GetObjectItem(json, "plugins");
    if (cJSON_IsArray(plugins)) {
        cJSON* plugin_item;
        cJSON_ArrayForEach(plugin_item, plugins) {
            // Support both "plugin_id" (preferred) and "id" (legacy) field names
            cJSON* id_item = cJSON_GetObjectItem(plugin_item, "plugin_id");
            if (!cJSON_IsString(id_item)) {
                id_item = cJSON_GetObjectItem(plugin_item, "id");
            }
            if (!cJSON_IsString(id_item)) {
                log_warn("[Config] Plugin missing plugin_id and id field, skipping");
                continue;
            }

            const char* plugin_id = id_item->valuestring;
            PluginConfig* pc = config_add_plugin_config(cfg, plugin_id);
            if (pc) {
                // Get the inner "config" object from the plugin item
                cJSON* inner_config = cJSON_GetObjectItem(plugin_item, "config");
                if (cJSON_IsObject(inner_config)) {
                    // Deep copy just the inner config object
                    cJSON* config_copy = cJSON_Duplicate(inner_config, 1);
                    if (config_copy) {
                        if (pc->config) {
                            cJSON_Delete(pc->config);
                        }
                        pc->config = config_copy;
                    }
                }
            }
        }
    }

    cJSON_Delete(json);

    // Apply environment variable overrides (env takes precedence over file)
    config_load_from_env(cfg);

    return true;
}

void config_load_from_env(Config* cfg) {
    if (!cfg) return;

    // Agent config - PRIMAGEN_AGENT_*
    const char* env_model = get_env_string("PRIMAGEN_AGENT_MODEL", NULL);
    if (env_model) { free(cfg->agent.model); cfg->agent.model = strdup(env_model); }

    const char* env_api_key = get_env_string("PRIMAGEN_AGENT_API_KEY", NULL);
    if (env_api_key) { free(cfg->agent.api_key); cfg->agent.api_key = strdup(env_api_key); }

    const char* env_api_base = get_env_string("PRIMAGEN_AGENT_API_BASE", NULL);
    if (env_api_base) { free(cfg->agent.api_base); cfg->agent.api_base = strdup(env_api_base); }

    const char* env_temperature = get_env_string("PRIMAGEN_AGENT_TEMPERATURE", NULL);
    if (env_temperature) { cfg->agent.temperature = get_env_double("PRIMAGEN_AGENT_TEMPERATURE", cfg->agent.temperature); }

    const char* env_max_tokens = get_env_string("PRIMAGEN_AGENT_MAX_TOKENS", NULL);
    if (env_max_tokens) { cfg->agent.max_tokens = get_env_int("PRIMAGEN_AGENT_MAX_TOKENS", cfg->agent.max_tokens); }

    const char* env_max_tool_iterations = get_env_string("PRIMAGEN_AGENT_MAX_TOOL_ITERATIONS", NULL);
    if (env_max_tool_iterations) { cfg->agent.max_tool_iterations = get_env_int("PRIMAGEN_AGENT_MAX_TOOL_ITERATIONS", cfg->agent.max_tool_iterations); }

    const char* env_memory_window = get_env_string("PRIMAGEN_AGENT_MEMORY_WINDOW", NULL);
    if (env_memory_window) { cfg->agent.memory_window = get_env_int("PRIMAGEN_AGENT_MEMORY_WINDOW", cfg->agent.memory_window); }

    const char* env_reasoning_effort = get_env_string("PRIMAGEN_AGENT_REASONING_EFFORT", NULL);
    if (env_reasoning_effort) { free(cfg->agent.reasoning_effort); cfg->agent.reasoning_effort = strdup(env_reasoning_effort); }

    // Tools config - PRIMAGEN_TOOLS_*
    const char* env_restrict = get_env_string("PRIMAGEN_TOOLS_RESTRICT_TO_WORKSPACE", NULL);
    if (env_restrict) { cfg->tools.restrict_to_workspace = get_env_bool("PRIMAGEN_TOOLS_RESTRICT_TO_WORKSPACE", cfg->tools.restrict_to_workspace); }

    const char* env_timeout = get_env_string("PRIMAGEN_TOOLS_EXEC_TIMEOUT", NULL);
    if (env_timeout) { cfg->tools.exec.timeout = get_env_int("PRIMAGEN_TOOLS_EXEC_TIMEOUT", cfg->tools.exec.timeout); }

    const char* env_exec_restrict = get_env_string("PRIMAGEN_TOOLS_EXEC_RESTRICT_TO_WORKSPACE", NULL);
    if (env_exec_restrict) { cfg->tools.exec.restrict_to_workspace = get_env_bool("PRIMAGEN_TOOLS_EXEC_RESTRICT_TO_WORKSPACE", cfg->tools.exec.restrict_to_workspace); }

    const char* env_path_append = get_env_string("PRIMAGEN_TOOLS_EXEC_PATH_APPEND", NULL);
    if (env_path_append) { free(cfg->tools.exec.path_append); cfg->tools.exec.path_append = strdup(env_path_append); }

    const char* env_web_proxy = get_env_string("PRIMAGEN_TOOLS_WEB_PROXY", NULL);
    if (env_web_proxy) { free(cfg->tools.web.proxy); cfg->tools.web.proxy = strdup(env_web_proxy); }

    const char* env_search_api_key = get_env_string("PRIMAGEN_TOOLS_WEB_SEARCH_API_KEY", NULL);
    if (env_search_api_key) { free(cfg->tools.web.search.api_key); cfg->tools.web.search.api_key = strdup(env_search_api_key); }

    // Heartbeat config - PRIMAGEN_HEARTBEAT_*
    const char* env_hb_enabled = get_env_string("PRIMAGEN_HEARTBEAT_ENABLED", NULL);
    if (env_hb_enabled) { cfg->heartbeat.enabled = get_env_bool("PRIMAGEN_HEARTBEAT_ENABLED", cfg->heartbeat.enabled); }

    const char* env_hb_interval = get_env_string("PRIMAGEN_HEARTBEAT_INTERVAL_S", NULL);
    if (env_hb_interval) { cfg->heartbeat.interval_s = get_env_int("PRIMAGEN_HEARTBEAT_INTERVAL_S", cfg->heartbeat.interval_s); }

    // Log config - PRIMAGEN_LOG_*
    const char* env_log_level = get_env_string("PRIMAGEN_LOG_LEVEL", NULL);
    if (env_log_level) { free(cfg->log.level); cfg->log.level = strdup(env_log_level); }

    const char* env_console_output = get_env_string("PRIMAGEN_LOG_CONSOLE_OUTPUT", NULL);
    if (env_console_output) { cfg->log.console_output = get_env_bool("PRIMAGEN_LOG_CONSOLE_OUTPUT", cfg->log.console_output); }

    // Channels config - PRIMAGEN_CHANNELS_*
    const char* env_send_progress = get_env_string("PRIMAGEN_CHANNELS_SEND_PROGRESS", NULL);
    if (env_send_progress) { cfg->channels.send_progress = get_env_bool("PRIMAGEN_CHANNELS_SEND_PROGRESS", cfg->channels.send_progress); }

    const char* env_send_tool_hints = get_env_string("PRIMAGEN_CHANNELS_SEND_TOOL_HINTS", NULL);
    if (env_send_tool_hints) { cfg->channels.send_tool_hints = get_env_bool("PRIMAGEN_CHANNELS_SEND_TOOL_HINTS", cfg->channels.send_tool_hints); }

    // Telegram - PRIMAGEN_TELEGRAM_*
    const char* env_tg_enabled = get_env_string("PRIMAGEN_TELEGRAM_ENABLED", NULL);
    if (env_tg_enabled) { cfg->channels.telegram.enabled = get_env_bool("PRIMAGEN_TELEGRAM_ENABLED", cfg->channels.telegram.enabled); }

    const char* env_tg_token = get_env_string("PRIMAGEN_TELEGRAM_TOKEN", NULL);
    if (env_tg_token) { free(cfg->channels.telegram.token); cfg->channels.telegram.token = strdup(env_tg_token); }

    // Email - PRIMAGEN_EMAIL_*
    const char* env_email_enabled = get_env_string("PRIMAGEN_EMAIL_ENABLED", NULL);
    if (env_email_enabled) { cfg->channels.email.enabled = get_env_bool("PRIMAGEN_EMAIL_ENABLED", cfg->channels.email.enabled); }

    const char* env_imap_host = get_env_string("PRIMAGEN_EMAIL_IMAP_HOST", NULL);
    if (env_imap_host) { free(cfg->channels.email.imap_host); cfg->channels.email.imap_host = strdup(env_imap_host); }

    const char* env_imap_port = get_env_string("PRIMAGEN_EMAIL_IMAP_PORT", NULL);
    if (env_imap_port) { cfg->channels.email.imap_port = get_env_int("PRIMAGEN_EMAIL_IMAP_PORT", cfg->channels.email.imap_port); }

    const char* env_imap_user = get_env_string("PRIMAGEN_EMAIL_IMAP_USERNAME", NULL);
    if (env_imap_user) { free(cfg->channels.email.imap_username); cfg->channels.email.imap_username = strdup(env_imap_user); }

    const char* env_imap_pass = get_env_string("PRIMAGEN_EMAIL_IMAP_PASSWORD", NULL);
    if (env_imap_pass) { free(cfg->channels.email.imap_password); cfg->channels.email.imap_password = strdup(env_imap_pass); }

    const char* env_smtp_host = get_env_string("PRIMAGEN_EMAIL_SMTP_HOST", NULL);
    if (env_smtp_host) { free(cfg->channels.email.smtp_host); cfg->channels.email.smtp_host = strdup(env_smtp_host); }

    const char* env_smtp_port = get_env_string("PRIMAGEN_EMAIL_SMTP_PORT", NULL);
    if (env_smtp_port) { cfg->channels.email.smtp_port = get_env_int("PRIMAGEN_EMAIL_SMTP_PORT", cfg->channels.email.smtp_port); }

    const char* env_smtp_user = get_env_string("PRIMAGEN_EMAIL_SMTP_USERNAME", NULL);
    if (env_smtp_user) { free(cfg->channels.email.smtp_username); cfg->channels.email.smtp_username = strdup(env_smtp_user); }

    const char* env_smtp_pass = get_env_string("PRIMAGEN_EMAIL_SMTP_PASSWORD", NULL);
    if (env_smtp_pass) { free(cfg->channels.email.smtp_password); cfg->channels.email.smtp_password = strdup(env_smtp_pass); }

    const char* env_from_addr = get_env_string("PRIMAGEN_EMAIL_FROM_ADDRESS", NULL);
    if (env_from_addr) { free(cfg->channels.email.from_address); cfg->channels.email.from_address = strdup(env_from_addr); }

    // Discord - PRIMAGEN_DISCORD_*
    const char* env_discord_enabled = get_env_string("PRIMAGEN_DISCORD_ENABLED", NULL);
    if (env_discord_enabled) { cfg->channels.discord.enabled = get_env_bool("PRIMAGEN_DISCORD_ENABLED", cfg->channels.discord.enabled); }

    const char* env_discord_token = get_env_string("PRIMAGEN_DISCORD_TOKEN", NULL);
    if (env_discord_token) { free(cfg->channels.discord.token); cfg->channels.discord.token = strdup(env_discord_token); }

    const char* env_discord_gateway = get_env_string("PRIMAGEN_DISCORD_GATEWAY_URL", NULL);
    if (env_discord_gateway) { free(cfg->channels.discord.gateway_url); cfg->channels.discord.gateway_url = strdup(env_discord_gateway); }

    // Slack - PRIMAGEN_SLACK_*
    const char* env_slack_enabled = get_env_string("PRIMAGEN_SLACK_ENABLED", NULL);
    if (env_slack_enabled) { cfg->channels.slack.enabled = get_env_bool("PRIMAGEN_SLACK_ENABLED", cfg->channels.slack.enabled); }

    const char* env_slack_bot_token = get_env_string("PRIMAGEN_SLACK_BOT_TOKEN", NULL);
    if (env_slack_bot_token) { free(cfg->channels.slack.bot_token); cfg->channels.slack.bot_token = strdup(env_slack_bot_token); }

    const char* env_slack_app_token = get_env_string("PRIMAGEN_SLACK_APP_TOKEN", NULL);
    if (env_slack_app_token) { free(cfg->channels.slack.app_token); cfg->channels.slack.app_token = strdup(env_slack_app_token); }

    // DingTalk - PRIMAGEN_DINGTALK_*
    const char* env_dingtalk_enabled = get_env_string("PRIMAGEN_DINGTALK_ENABLED", NULL);
    if (env_dingtalk_enabled) { cfg->channels.dingtalk.enabled = get_env_bool("PRIMAGEN_DINGTALK_ENABLED", cfg->channels.dingtalk.enabled); }

    const char* env_dingtalk_client_id = get_env_string("PRIMAGEN_DINGTALK_CLIENT_ID", NULL);
    if (env_dingtalk_client_id) { free(cfg->channels.dingtalk.client_id); cfg->channels.dingtalk.client_id = strdup(env_dingtalk_client_id); }

    const char* env_dingtalk_client_secret = get_env_string("PRIMAGEN_DINGTALK_CLIENT_SECRET", NULL);
    if (env_dingtalk_client_secret) { free(cfg->channels.dingtalk.client_secret); cfg->channels.dingtalk.client_secret = strdup(env_dingtalk_client_secret); }

    // WhatsApp - PRIMAGEN_WHATSAPP_*
    const char* env_whatsapp_enabled = get_env_string("PRIMAGEN_WHATSAPP_ENABLED", NULL);
    if (env_whatsapp_enabled) { cfg->channels.whatsapp.enabled = get_env_bool("PRIMAGEN_WHATSAPP_ENABLED", cfg->channels.whatsapp.enabled); }

    const char* env_whatsapp_bridge_url = get_env_string("PRIMAGEN_WHATSAPP_BRIDGE_URL", NULL);
    if (env_whatsapp_bridge_url) { free(cfg->channels.whatsapp.bridge_url); cfg->channels.whatsapp.bridge_url = strdup(env_whatsapp_bridge_url); }

    const char* env_whatsapp_bridge_token = get_env_string("PRIMAGEN_WHATSAPP_BRIDGE_TOKEN", NULL);
    if (env_whatsapp_bridge_token) { free(cfg->channels.whatsapp.bridge_token); cfg->channels.whatsapp.bridge_token = strdup(env_whatsapp_bridge_token); }
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

    // Plugins
    if (cfg->plugins.count > 0) {
        cJSON* plugins_array = cJSON_CreateArray();
        for (size_t i = 0; i < cfg->plugins.count; i++) {
            PluginConfig* pc = &cfg->plugins.items[i];
            if (pc->plugin_id) {
                cJSON* plugin_obj = cJSON_CreateObject();
                cJSON_AddStringToObject(plugin_obj, "id", pc->plugin_id);
                if (pc->config) {
                    // Merge config fields into the plugin object
                    cJSON* child = pc->config->child;
                    while (child) {
                        cJSON* copy = cJSON_Duplicate(child, 1);
                        if (copy) {
                            cJSON_AddItemReferenceToObject(plugin_obj, child->string, copy);
                            cJSON_Delete(copy);  // Remove reference, item is now owned by plugin_obj
                        }
                        child = child->next;
                    }
                }
                cJSON_AddItemToArray(plugins_array, plugin_obj);
            }
        }
        cJSON_AddItemToObject(json, "plugins", plugins_array);
    }

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
