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

    // Allocate plugins on heap (not stack!) so they persist after function returns
    cfg->plugins.items = malloc(2 * sizeof(PluginConfig));
    cfg->plugins.count = 2;
    cfg->plugins.capacity = 2;

    cfg->plugins.items[0].plugin_id = strdup("feishu_channel");
    cfg->plugins.items[0].config = cJSON_CreateObject();
    cJSON_AddBoolToObject(cfg->plugins.items[0].config, "enabled", false);
    cJSON_AddStringToObject(cfg->plugins.items[0].config, "app_id", "");
    cJSON_AddStringToObject(cfg->plugins.items[0].config, "app_secret", "");
    cJSON_AddBoolToObject(cfg->plugins.items[0].config, "use_card", false);
    cJSON_AddNullToObject(cfg->plugins.items[0].config, "allow_from");

    cfg->plugins.items[1].plugin_id = strdup("dingtalk_channel");
    cfg->plugins.items[1].config = cJSON_CreateObject();
    cJSON_AddBoolToObject(cfg->plugins.items[1].config, "enabled", false);
    cJSON_AddStringToObject(cfg->plugins.items[1].config, "clientId", "");
    cJSON_AddStringToObject(cfg->plugins.items[1].config, "clientSecret", "");
    cJSON_AddBoolToObject(cfg->plugins.items[1].config, "use_card", false);
    cJSON_AddNullToObject(cfg->plugins.items[1].config, "allow_from");

    // Default log config
    cfg->log.level = strdup("INFO");
    cfg->log.console_output = false;

    // Default MCP config
    cfg->mcp.enabled = false;
    cfg->mcp.servers = NULL;
    cfg->mcp.server_count = 0;
    cfg->mcp.server_capacity = 0;

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
        if ((item = cJSON_GetObjectItem(agent, "memory_max_tokens"))) cfg->agent.memory_max_tokens = get_json_int(item, 4000);
        if ((item = cJSON_GetObjectItem(agent, "memory_consolidation_threshold"))) cfg->agent.memory_consolidation_threshold = get_json_double(item, 0.8);
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
