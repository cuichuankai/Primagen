#include "../include/config.h"
#include "../include/logger.h"
#include "../vendor/cJSON/cJSON.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
    return (strcmp(val, "true") == 0 || strcmp(val, "yes") == 0 || strcmp(val, "1") == 0);
}

static char* get_json_string(cJSON* item, const char* default_val) {
    if (cJSON_IsString(item) && (item->valuestring != NULL)) {
        return strdup(item->valuestring);
    }
    return strdup(default_val);
}

static int get_json_int(cJSON* item, int default_val) {
    if (cJSON_IsNumber(item)) return item->valueint;
    return default_val;
}

static double get_json_double(cJSON* item, double default_val) {
    if (cJSON_IsNumber(item)) return item->valuedouble;
    return default_val;
}

static bool get_json_bool(cJSON* item, bool default_val) {
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    return default_val;
}

// =============================================================================
// ProviderConfig helpers
// =============================================================================

ProviderConfig* config_add_provider(Config* cfg, const char* name) {
    if (!cfg || !name) return NULL;
    for (size_t i = 0; i < cfg->providers.count; i++) {
        if (cfg->providers.items[i].name && strcmp(cfg->providers.items[i].name, name) == 0)
            return &cfg->providers.items[i];
    }
    if (cfg->providers.count >= cfg->providers.capacity) {
        size_t new_capacity = cfg->providers.capacity == 0 ? 4 : cfg->providers.capacity * 2;
        ProviderConfig* new_items = realloc(cfg->providers.items, new_capacity * sizeof(ProviderConfig));
        if (!new_items) return NULL;
        cfg->providers.items = new_items;
        cfg->providers.capacity = new_capacity;
    }
    ProviderConfig* item = &cfg->providers.items[cfg->providers.count];
    item->name = strdup(name);
    item->model = strdup("");
    item->api_key = strdup("");
    item->api_base = strdup("");
    item->temperature = 0.1;
    item->max_tokens = 4096;
    item->reasoning_effort = strdup("medium");
    cfg->providers.count++;
    return item;
}

ProviderConfig* config_get_provider(Config* cfg, const char* name) {
    if (!cfg || !name) return NULL;
    for (size_t i = 0; i < cfg->providers.count; i++) {
        if (cfg->providers.items[i].name && strcmp(cfg->providers.items[i].name, name) == 0)
            return &cfg->providers.items[i];
    }
    return NULL;
}

ProviderConfig* config_get_active_provider(Config* cfg) {
    if (!cfg) return NULL;
    return config_get_provider(cfg, cfg->agent.provider);
}

void config_provider_config_free(ProviderConfig* item) {
    if (!item) return;
    free(item->name);
    free(item->model);
    free(item->api_key);
    free(item->api_base);
    free(item->reasoning_effort);
    item->name = NULL;
    item->model = NULL;
    item->api_key = NULL;
    item->api_base = NULL;
    item->reasoning_effort = NULL;
}

// =============================================================================
// PluginConfig helpers
// =============================================================================

PluginConfig* config_add_plugin_config(Config* cfg, const char* plugin_id) {
    if (!cfg || !plugin_id) return NULL;
    for (size_t i = 0; i < cfg->plugins.count; i++) {
        if (cfg->plugins.items[i].plugin_id && strcmp(cfg->plugins.items[i].plugin_id, plugin_id) == 0)
            return &cfg->plugins.items[i];
    }
    if (cfg->plugins.count >= cfg->plugins.capacity) {
        size_t new_capacity = cfg->plugins.capacity == 0 ? 4 : cfg->plugins.capacity * 2;
        PluginConfig* new_items = realloc(cfg->plugins.items, new_capacity * sizeof(PluginConfig));
        if (!new_items) return NULL;
        cfg->plugins.items = new_items;
        cfg->plugins.capacity = new_capacity;
    }
    PluginConfig* item = &cfg->plugins.items[cfg->plugins.count];
    item->plugin_id = strdup(plugin_id);
    item->enabled = false;
    item->config = NULL;
    cfg->plugins.count++;
    return item;
}

PluginConfig* config_get_plugin_config(Config* cfg, const char* plugin_id) {
    if (!cfg || !plugin_id) return NULL;
    for (size_t i = 0; i < cfg->plugins.count; i++) {
        if (cfg->plugins.items[i].plugin_id && strcmp(cfg->plugins.items[i].plugin_id, plugin_id) == 0)
            return &cfg->plugins.items[i];
    }
    return NULL;
}

void config_plugin_config_free(PluginConfig* item) {
    if (!item) return;
    free(item->plugin_id);
    if (item->config) cJSON_Delete(item->config);
    item->plugin_id = NULL;
    item->config = NULL;
}

// =============================================================================
// Config create / destroy
// =============================================================================

Config* config_create() {
    Config* cfg = malloc(sizeof(Config));
    if (!cfg) return NULL;

    cfg->agent.provider = strdup("openai");
    cfg->agent.max_tool_iterations = 15;
    cfg->agent.memory_window = 100;
    cfg->agent.memory_max_tokens = 4000;
    cfg->agent.memory_consolidation_threshold = 0.8;

    cfg->providers.items = NULL;
    cfg->providers.count = 0;
    cfg->providers.capacity = 0;

    ProviderConfig* default_provider = config_add_provider(cfg, "openai");
    if (default_provider) {
        free(default_provider->model); default_provider->model = strdup("gpt-4");
        free(default_provider->api_base); default_provider->api_base = strdup("https://api.openai.com/v1");
    }

    cfg->tools.exec.timeout = 300;
    cfg->tools.exec.restrict_to_workspace = true;
    cfg->tools.exec.path_append = strdup("");
    cfg->tools.restrict_to_workspace = true;

    cfg->dns.dns4 = strdup("8.8.8.8");
    cfg->dns.dns6 = strdup("2001:4860:4860::8888");
    cfg->dns.dns4_url = strdup("udp://8.8.8.8:53");
    cfg->dns.dns6_url = strdup("udp://2001:4860:4860::8888:53");
    cfg->dns.dns_timeout_ms = 5000;
    cfg->dns.use_system_resolver = false;

    cfg->heartbeat.enabled = true;
    cfg->heartbeat.interval_s = 300;

    cfg->plugins.items = NULL;
    cfg->plugins.count = 0;
    cfg->plugins.capacity = 0;

    cfg->log.level = strdup("INFO");
    cfg->log.console_output = false;

    return cfg;
}

void config_destroy(Config* cfg) {
    if (!cfg) return;

    free(cfg->agent.provider);
    for (size_t i = 0; i < cfg->providers.count; i++) config_provider_config_free(&cfg->providers.items[i]);
    free(cfg->providers.items);
    free(cfg->tools.exec.path_append);
    free(cfg->dns.dns4);
    free(cfg->dns.dns6);
    free(cfg->dns.dns4_url);
    free(cfg->dns.dns6_url);
    free(cfg->log.level);
    for (size_t i = 0; i < cfg->plugins.count; i++) config_plugin_config_free(&cfg->plugins.items[i]);
    free(cfg->plugins.items);
    free(cfg);
}

AgentConfig* config_get_agent_config(Config* cfg) { return cfg ? &cfg->agent : NULL; }
ToolConfig* config_get_tool_config(Config* cfg) { return cfg ? &cfg->tools : NULL; }
DNSConfig* config_get_dns_config(Config* cfg) { return cfg ? &cfg->dns : NULL; }
HeartbeatConfig* config_get_heartbeat_config(Config* cfg) { return cfg ? &cfg->heartbeat : NULL; }

// =============================================================================
// Config load from file
// =============================================================================

bool config_load_from_file(Config* cfg, const char* filepath) {
    FILE* fp = fopen(filepath, "r");
    if (!fp) return true;

    fseek(fp, 0, SEEK_END);
    long length = ftell(fp);
    if (length < 0) { fclose(fp); return false; }
    fseek(fp, 0, SEEK_SET);

    char* data = malloc((size_t)length + 1);
    if (!data) { fclose(fp); return false; }
    if (fread(data, 1, length, fp) != (size_t)length) { free(data); fclose(fp); return false; }
    data[length] = '\0';
    fclose(fp);

    cJSON* json = cJSON_Parse(data);
    free(data);
    if (!json) return false;

    // Agent Config (only provider selection + agent-level settings)
    cJSON* agent = cJSON_GetObjectItem(json, "agent");
    if (agent) {
        cJSON* item;
        if ((item = cJSON_GetObjectItem(agent, "provider"))) {
            free(cfg->agent.provider);
            cfg->agent.provider = get_json_string(item, "openai");
        }
        if ((item = cJSON_GetObjectItem(agent, "max_tool_iterations"))) cfg->agent.max_tool_iterations = get_json_int(item, 15);
        if ((item = cJSON_GetObjectItem(agent, "memory_window"))) cfg->agent.memory_window = get_json_int(item, 100);
        if ((item = cJSON_GetObjectItem(agent, "memory_max_tokens"))) cfg->agent.memory_max_tokens = get_json_int(item, 4000);
        if ((item = cJSON_GetObjectItem(agent, "memory_consolidation_threshold"))) cfg->agent.memory_consolidation_threshold = get_json_double(item, 0.8);

        // Backward compat: if old-style agent fields exist, merge into "openai" provider
        cJSON* model = cJSON_GetObjectItem(agent, "model");
        cJSON* api_key = cJSON_GetObjectItem(agent, "apiKey");
        cJSON* api_base = cJSON_GetObjectItem(agent, "apiBase");
        cJSON* temperature = cJSON_GetObjectItem(agent, "temperature");
        cJSON* max_tokens = cJSON_GetObjectItem(agent, "max_tokens");
        cJSON* reasoning_effort = cJSON_GetObjectItem(agent, "reasoning_effort");

        if (model || api_key || api_base || temperature || max_tokens || reasoning_effort) {
            ProviderConfig* pc = config_get_provider(cfg, "openai");
            if (!pc) pc = config_add_provider(cfg, "openai");
            if (pc) {
                if (model) { free(pc->model); pc->model = get_json_string(model, pc->model); }
                if (api_key) { free(pc->api_key); pc->api_key = get_json_string(api_key, pc->api_key); }
                if (api_base) { free(pc->api_base); pc->api_base = get_json_string(api_base, pc->api_base); }
                if (temperature) pc->temperature = get_json_double(temperature, pc->temperature);
                if (max_tokens) pc->max_tokens = get_json_int(max_tokens, pc->max_tokens);
                if (reasoning_effort) { free(pc->reasoning_effort); pc->reasoning_effort = get_json_string(reasoning_effort, pc->reasoning_effort); }
            }
        }
    }

    // Providers Config (array)
    cJSON* providers = cJSON_GetObjectItem(json, "providers");
    if (cJSON_IsArray(providers)) {
        cJSON* provider_entry = NULL;
        cJSON_ArrayForEach(provider_entry, providers) {
            if (!cJSON_IsObject(provider_entry)) continue;

            cJSON* name_item = cJSON_GetObjectItem(provider_entry, "name");
            if (!cJSON_IsString(name_item) || !name_item->valuestring) continue;
            const char* provider_name = name_item->valuestring;

            ProviderConfig* pc = config_get_provider(cfg, provider_name);
            if (!pc) pc = config_add_provider(cfg, provider_name);
            if (!pc) continue;

            cJSON* item;
            if ((item = cJSON_GetObjectItem(provider_entry, "model"))) { free(pc->model); pc->model = get_json_string(item, pc->model); }
            if ((item = cJSON_GetObjectItem(provider_entry, "apiKey"))) { free(pc->api_key); pc->api_key = get_json_string(item, pc->api_key); }
            if ((item = cJSON_GetObjectItem(provider_entry, "apiBase"))) { free(pc->api_base); pc->api_base = get_json_string(item, pc->api_base); }
            if ((item = cJSON_GetObjectItem(provider_entry, "temperature"))) pc->temperature = get_json_double(item, pc->temperature);
            if ((item = cJSON_GetObjectItem(provider_entry, "max_tokens"))) pc->max_tokens = get_json_int(item, pc->max_tokens);
            if ((item = cJSON_GetObjectItem(provider_entry, "reasoning_effort"))) { free(pc->reasoning_effort); pc->reasoning_effort = get_json_string(item, pc->reasoning_effort); }
        }
    }

    // Tools Config
    cJSON* tools = cJSON_GetObjectItem(json, "tools");
    if (tools) {
        cJSON* item;
        if ((item = cJSON_GetObjectItem(tools, "restrictToWorkspace"))) cfg->tools.restrict_to_workspace = get_json_bool(item, true);
        cJSON* exec = cJSON_GetObjectItem(tools, "exec");
        if (exec) {
            if ((item = cJSON_GetObjectItem(exec, "timeout"))) cfg->tools.exec.timeout = get_json_int(item, 300);
            if ((item = cJSON_GetObjectItem(exec, "restrictToWorkspace"))) cfg->tools.exec.restrict_to_workspace = get_json_bool(item, true);
            if ((item = cJSON_GetObjectItem(exec, "pathAppend"))) { free(cfg->tools.exec.path_append); cfg->tools.exec.path_append = get_json_string(item, ""); }
        }
    }

    // DNS Config
    cJSON* dns = cJSON_GetObjectItem(json, "dns");
    if (dns) {
        cJSON* item;
        if ((item = cJSON_GetObjectItem(dns, "dns4"))) { free(cfg->dns.dns4); cfg->dns.dns4 = NULL; if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) cfg->dns.dns4 = strdup(item->valuestring); }
        if ((item = cJSON_GetObjectItem(dns, "dns6"))) { free(cfg->dns.dns6); cfg->dns.dns6 = NULL; if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) cfg->dns.dns6 = strdup(item->valuestring); }
        if ((item = cJSON_GetObjectItem(dns, "dnsTimeoutMs"))) { int t = get_json_int(item, 0); cfg->dns.dns_timeout_ms = t > 0 ? t : 0; }
        if ((item = cJSON_GetObjectItem(dns, "useSystemResolver"))) cfg->dns.use_system_resolver = get_json_bool(item, false);
    }
    free(cfg->dns.dns4_url); cfg->dns.dns4_url = NULL;
    if (cfg->dns.dns4 && cfg->dns.dns4[0]) { size_t len = strlen(cfg->dns.dns4) + 16; cfg->dns.dns4_url = malloc(len); if (cfg->dns.dns4_url) snprintf(cfg->dns.dns4_url, len, "udp://%s:53", cfg->dns.dns4); }
    free(cfg->dns.dns6_url); cfg->dns.dns6_url = NULL;
    if (cfg->dns.dns6 && cfg->dns.dns6[0]) { size_t len = strlen(cfg->dns.dns6) + 16; cfg->dns.dns6_url = malloc(len); if (cfg->dns.dns6_url) snprintf(cfg->dns.dns6_url, len, "udp://%s:53", cfg->dns.dns6); }

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
        if ((item = cJSON_GetObjectItem(log, "level"))) { free(cfg->log.level); cfg->log.level = get_json_string(item, "INFO"); }
        if ((item = cJSON_GetObjectItem(log, "consoleOutput"))) cfg->log.console_output = get_json_bool(item, true);
    }

    // Plugins Config
    cJSON* plugins = cJSON_GetObjectItem(json, "plugins");
    if (cJSON_IsArray(plugins)) {
        cJSON* plugin_item;
        cJSON_ArrayForEach(plugin_item, plugins) {
            cJSON* id_item = cJSON_GetObjectItem(plugin_item, "plugin_id");
            if (!cJSON_IsString(id_item)) continue;
            cJSON* inner_config = cJSON_GetObjectItem(plugin_item, "config");
            if (!cJSON_IsObject(inner_config)) continue;
            const char* plugin_id = id_item->valuestring;
            PluginConfig* pc = config_add_plugin_config(cfg, plugin_id);
            if (pc) {
                cJSON* enabled_item = cJSON_GetObjectItem(plugin_item, "enabled");
                pc->enabled = cJSON_IsBool(enabled_item) ? cJSON_IsTrue(enabled_item) : false;
                cJSON* config_copy = cJSON_Duplicate(inner_config, 1);
                if (config_copy) { if (pc->config) cJSON_Delete(pc->config); pc->config = config_copy; }
            }
        }
    }

    cJSON_Delete(json);
    config_load_from_env(cfg);
    return true;
}

// =============================================================================
// Config load from env
// =============================================================================

void config_load_from_env(Config* cfg) {
    if (!cfg) return;

    const char* env_provider = get_env_string("PRIMAGEN_AGENT_PROVIDER", NULL);
    if (env_provider) { free(cfg->agent.provider); cfg->agent.provider = strdup(env_provider); }

    const char* env_max_tool_iterations = get_env_string("PRIMAGEN_AGENT_MAX_TOOL_ITERATIONS", NULL);
    if (env_max_tool_iterations) cfg->agent.max_tool_iterations = get_env_int("PRIMAGEN_AGENT_MAX_TOOL_ITERATIONS", cfg->agent.max_tool_iterations);

    const char* env_memory_window = get_env_string("PRIMAGEN_AGENT_MEMORY_WINDOW", NULL);
    if (env_memory_window) cfg->agent.memory_window = get_env_int("PRIMAGEN_AGENT_MEMORY_WINDOW", cfg->agent.memory_window);

    // Provider-specific env vars: PRIMAGEN_PROVIDER_<NAME>_<FIELD>
    for (size_t i = 0; i < cfg->providers.count; i++) {
        ProviderConfig* pc = &cfg->providers.items[i];
        char env_key[256];

        snprintf(env_key, sizeof(env_key), "PRIMAGEN_PROVIDER_%s_MODEL", pc->name);
        const char* v = get_env_string(env_key, NULL);
        if (v) { free(pc->model); pc->model = strdup(v); }

        snprintf(env_key, sizeof(env_key), "PRIMAGEN_PROVIDER_%s_API_KEY", pc->name);
        v = get_env_string(env_key, NULL);
        if (v) { free(pc->api_key); pc->api_key = strdup(v); }

        snprintf(env_key, sizeof(env_key), "PRIMAGEN_PROVIDER_%s_API_BASE", pc->name);
        v = get_env_string(env_key, NULL);
        if (v) { free(pc->api_base); pc->api_base = strdup(v); }

        snprintf(env_key, sizeof(env_key), "PRIMAGEN_PROVIDER_%s_TEMPERATURE", pc->name);
        v = get_env_string(env_key, NULL);
        if (v) pc->temperature = atof(v);

        snprintf(env_key, sizeof(env_key), "PRIMAGEN_PROVIDER_%s_MAX_TOKENS", pc->name);
        v = get_env_string(env_key, NULL);
        if (v) pc->max_tokens = atoi(v);

        snprintf(env_key, sizeof(env_key), "PRIMAGEN_PROVIDER_%s_REASONING_EFFORT", pc->name);
        v = get_env_string(env_key, NULL);
        if (v) { free(pc->reasoning_effort); pc->reasoning_effort = strdup(v); }
    }

    // Legacy env var compat: PRIMAGEN_AGENT_MODEL etc. -> openai provider
    const char* env_model = get_env_string("PRIMAGEN_AGENT_MODEL", NULL);
    const char* env_api_key = get_env_string("PRIMAGEN_AGENT_API_KEY", NULL);
    const char* env_api_base = get_env_string("PRIMAGEN_AGENT_API_BASE", NULL);
    const char* env_temperature = get_env_string("PRIMAGEN_AGENT_TEMPERATURE", NULL);
    const char* env_max_tokens = get_env_string("PRIMAGEN_AGENT_MAX_TOKENS", NULL);
    const char* env_reasoning_effort = get_env_string("PRIMAGEN_AGENT_REASONING_EFFORT", NULL);

    if (env_model || env_api_key || env_api_base || env_temperature || env_max_tokens || env_reasoning_effort) {
        ProviderConfig* pc = config_get_provider(cfg, "openai");
        if (!pc) pc = config_add_provider(cfg, "openai");
        if (pc) {
            if (env_model) { free(pc->model); pc->model = strdup(env_model); }
            if (env_api_key) { free(pc->api_key); pc->api_key = strdup(env_api_key); }
            if (env_api_base) { free(pc->api_base); pc->api_base = strdup(env_api_base); }
            if (env_temperature) pc->temperature = atof(env_temperature);
            if (env_max_tokens) pc->max_tokens = atoi(env_max_tokens);
            if (env_reasoning_effort) { free(pc->reasoning_effort); pc->reasoning_effort = strdup(env_reasoning_effort); }
        }
    }

    // Tools config
    const char* env_restrict = get_env_string("PRIMAGEN_TOOLS_RESTRICT_TO_WORKSPACE", NULL);
    if (env_restrict) cfg->tools.restrict_to_workspace = get_env_bool("PRIMAGEN_TOOLS_RESTRICT_TO_WORKSPACE", cfg->tools.restrict_to_workspace);
    const char* env_timeout = get_env_string("PRIMAGEN_TOOLS_EXEC_TIMEOUT", NULL);
    if (env_timeout) cfg->tools.exec.timeout = get_env_int("PRIMAGEN_TOOLS_EXEC_TIMEOUT", cfg->tools.exec.timeout);
    const char* env_exec_restrict = get_env_string("PRIMAGEN_TOOLS_EXEC_RESTRICT_TO_WORKSPACE", NULL);
    if (env_exec_restrict) cfg->tools.exec.restrict_to_workspace = get_env_bool("PRIMAGEN_TOOLS_EXEC_RESTRICT_TO_WORKSPACE", cfg->tools.exec.restrict_to_workspace);
    const char* env_path_append = get_env_string("PRIMAGEN_TOOLS_EXEC_PATH_APPEND", NULL);
    if (env_path_append) { free(cfg->tools.exec.path_append); cfg->tools.exec.path_append = strdup(env_path_append); }
    const char* env_dns_use_system = get_env_string("PRIMAGEN_DNS_USE_SYSTEM_RESOLVER", NULL);
    if (env_dns_use_system) cfg->dns.use_system_resolver = get_env_bool("PRIMAGEN_DNS_USE_SYSTEM_RESOLVER", cfg->dns.use_system_resolver);

    PluginConfig* web_tools = config_get_plugin_config(cfg, "web_tools");
    const char* env_web_proxy = get_env_string("PRIMAGEN_TOOLS_WEB_PROXY", NULL);
    if (env_web_proxy && web_tools && web_tools->config) { cJSON_DeleteItemFromObject(web_tools->config, "proxy"); cJSON_AddStringToObject(web_tools->config, "proxy", env_web_proxy); }
    const char* env_search_api_key = get_env_string("PRIMAGEN_TOOLS_WEB_SEARCH_API_KEY", NULL);
    if (env_search_api_key && web_tools && web_tools->config) { cJSON_DeleteItemFromObject(web_tools->config, "search_api_key"); cJSON_AddStringToObject(web_tools->config, "search_api_key", env_search_api_key); }

    const char* env_hb_enabled = get_env_string("PRIMAGEN_HEARTBEAT_ENABLED", NULL);
    if (env_hb_enabled) cfg->heartbeat.enabled = get_env_bool("PRIMAGEN_HEARTBEAT_ENABLED", cfg->heartbeat.enabled);
    const char* env_hb_interval = get_env_string("PRIMAGEN_HEARTBEAT_INTERVAL_S", NULL);
    if (env_hb_interval) cfg->heartbeat.interval_s = get_env_int("PRIMAGEN_HEARTBEAT_INTERVAL_S", cfg->heartbeat.interval_s);

    const char* env_log_level = get_env_string("PRIMAGEN_LOG_LEVEL", NULL);
    if (env_log_level) { free(cfg->log.level); cfg->log.level = strdup(env_log_level); }
    const char* env_console_output = get_env_string("PRIMAGEN_LOG_CONSOLE_OUTPUT", NULL);
    if (env_console_output) cfg->log.console_output = get_env_bool("PRIMAGEN_LOG_CONSOLE_OUTPUT", cfg->log.console_output);
}

// =============================================================================
// Validate / Save
// =============================================================================

Error config_validate(const Config* cfg) {
    if (!cfg) return error_new(ERR_INVALID_PARAM, "Config is NULL");
    if (!cfg->agent.provider || cfg->agent.provider[0] == '\0')
        return error_new(ERR_INVALID_PARAM, "agent.provider must not be empty");
    ProviderConfig* pc = config_get_provider((Config*)cfg, cfg->agent.provider);
    if (!pc) { char buf[128]; snprintf(buf, sizeof(buf), "provider config not found for: %s", cfg->agent.provider); return error_new(ERR_INVALID_PARAM, buf); }
    if (!pc->model || pc->model[0] == '\0') { char buf[128]; snprintf(buf, sizeof(buf), "provider %s: model must not be empty", cfg->agent.provider); return error_new(ERR_INVALID_PARAM, buf); }
    if (cfg->tools.exec.timeout <= 0)
        return error_new(ERR_INVALID_PARAM, "tool.timeout must be greater than 0");
    if (!cfg->log.level || cfg->log.level[0] == '\0')
        return error_new(ERR_INVALID_PARAM, "log.level must not be empty");
    return error_new(ERR_NONE, "");
}

bool config_save_to_file(Config* cfg, const char* filepath) {
    cJSON* json = cJSON_CreateObject();

    // Agent
    cJSON* agent = cJSON_CreateObject();
    cJSON_AddStringToObject(agent, "provider", cfg->agent.provider);
    cJSON_AddNumberToObject(agent, "max_tool_iterations", cfg->agent.max_tool_iterations);
    cJSON_AddNumberToObject(agent, "memory_window", cfg->agent.memory_window);
    cJSON_AddNumberToObject(agent, "memory_max_tokens", cfg->agent.memory_max_tokens);
    cJSON_AddNumberToObject(agent, "memory_consolidation_threshold", cfg->agent.memory_consolidation_threshold);
    cJSON_AddItemToObject(json, "agent", agent);

    // Providers (array)
    if (cfg->providers.count > 0) {
        cJSON* providers = cJSON_CreateArray();
        for (size_t i = 0; i < cfg->providers.count; i++) {
            ProviderConfig* pc = &cfg->providers.items[i];
            cJSON* p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "name", pc->name);
            cJSON_AddStringToObject(p, "model", pc->model);
            if (pc->api_key && strlen(pc->api_key) > 0)
                cJSON_AddStringToObject(p, "apiKey", "***REDACTED***");
            else
                cJSON_AddStringToObject(p, "apiKey", "");
            cJSON_AddStringToObject(p, "apiBase", pc->api_base);
            cJSON_AddNumberToObject(p, "temperature", pc->temperature);
            cJSON_AddNumberToObject(p, "max_tokens", pc->max_tokens);
            cJSON_AddStringToObject(p, "reasoning_effort", pc->reasoning_effort);
            cJSON_AddItemToArray(providers, p);
        }
        cJSON_AddItemToObject(json, "providers", providers);
    }

    // Tools
    cJSON* tools = cJSON_CreateObject();
    cJSON_AddBoolToObject(tools, "restrictToWorkspace", cfg->tools.restrict_to_workspace);
    cJSON* exec = cJSON_CreateObject();
    cJSON_AddNumberToObject(exec, "timeout", cfg->tools.exec.timeout);
    cJSON_AddBoolToObject(exec, "restrictToWorkspace", cfg->tools.exec.restrict_to_workspace);
    cJSON_AddStringToObject(exec, "pathAppend", cfg->tools.exec.path_append);
    cJSON_AddItemToObject(tools, "exec", exec);
    cJSON_AddItemToObject(json, "tools", tools);

    // DNS
    if ((cfg->dns.dns4 && cfg->dns.dns4[0]) || (cfg->dns.dns6 && cfg->dns.dns6[0]) || cfg->dns.dns_timeout_ms > 0 || cfg->dns.use_system_resolver) {
        cJSON* dns = cJSON_CreateObject();
        if (cfg->dns.dns4 && cfg->dns.dns4[0]) cJSON_AddStringToObject(dns, "dns4", cfg->dns.dns4);
        if (cfg->dns.dns6 && cfg->dns.dns6[0]) cJSON_AddStringToObject(dns, "dns6", cfg->dns.dns6);
        if (cfg->dns.dns_timeout_ms > 0) cJSON_AddNumberToObject(dns, "dnsTimeoutMs", cfg->dns.dns_timeout_ms);
        if (cfg->dns.use_system_resolver) cJSON_AddBoolToObject(dns, "useSystemResolver", true);
        cJSON_AddItemToObject(json, "dns", dns);
    }

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
                cJSON_AddStringToObject(plugin_obj, "plugin_id", pc->plugin_id);
                cJSON_AddBoolToObject(plugin_obj, "enabled", pc->enabled);
                if (pc->config) {
                    cJSON* config_copy = cJSON_Duplicate(pc->config, 1);
                    if (config_copy) { cJSON_DeleteItemFromObject(config_copy, "enabled"); cJSON_AddItemToObject(plugin_obj, "config", config_copy); }
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
    if (!fp) { free(string); return false; }
    fputs(string, fp);
    fclose(fp);
    free(string);
    return true;
}