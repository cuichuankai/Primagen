#include "../../../src/include/plugin.h"
#include "../../../src/plugin/plugin_manager.h"
#include "../../../src/include/config.h"
#include "../../../src/include/logger.h"
#include "../../../src/vendor/cJSON/cJSON.h"
#include "../../../src/vendor/mongoose/mongoose.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    PluginManager* manager;
} WebToolContext;

typedef struct {
    char* memory;
    size_t size;
    bool done;
} HttpChunk;

#define WEB_REQUEST_TIMEOUT_MS 30000

static void http_collect(struct mg_connection* c, int ev, void* ev_data) {
    HttpChunk* chunk = (HttpChunk*) c->fn_data;
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message* hm = (struct mg_http_message*) ev_data;
        size_t new_size = chunk->size + hm->body.len;
        char* new_mem = realloc(chunk->memory, new_size + 1);
        if (!new_mem) {
            chunk->done = true;
            c->is_closing = 1;
            return;
        }
        chunk->memory = new_mem;
        memcpy(chunk->memory + chunk->size, hm->body.buf, hm->body.len);
        chunk->size = new_size;
        chunk->memory[chunk->size] = '\0';
        chunk->done = true;
        c->is_closing = 1;
    } else if (ev == MG_EV_ERROR) {
        chunk->done = true;
    }
}

static int json_get_int(cJSON* root, const char* key, int default_val) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(item)) return item->valueint;
    return default_val;
}

static const char* json_get_str(cJSON* root, const char* key) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(item) && item->valuestring) return item->valuestring;
    return NULL;
}

static void strip_tags(const char* src, char* dst) {
    int in_tag = 0;
    while (*src) {
        if (*src == '<') {
            in_tag = 1;
        } else if (*src == '>') {
            in_tag = 0;
        } else if (!in_tag) {
            *dst++ = *src;
        }
        src++;
    }
    *dst = '\0';
}

static const cJSON* get_plugin_cfg(PluginManager* manager) {
    if (!manager || !manager->config) return NULL;
    PluginConfig* pc = config_get_plugin_config(manager->config, "web_tools");
    if (!pc || !pc->config) return NULL;
    return pc->config;
}

static bool get_search_enabled(PluginManager* manager) {
    bool enabled = true;
    if (manager && manager->config) {
        enabled = manager->config->tools.web.search.enabled;
    }
    const cJSON* cfg = get_plugin_cfg(manager);
    if (cfg) {
        cJSON* item = cJSON_GetObjectItem((cJSON*) cfg, "search_enabled");
        if (cJSON_IsBool(item)) enabled = cJSON_IsTrue(item);
    }
    return enabled;
}

static const char* get_search_api_key(PluginManager* manager) {
    const cJSON* cfg = get_plugin_cfg(manager);
    if (cfg) {
        cJSON* item = cJSON_GetObjectItem((cJSON*) cfg, "search_api_key");
        if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
            return item->valuestring;
        }
    }
    if (manager && manager->config && manager->config->tools.web.search.api_key &&
        manager->config->tools.web.search.api_key[0] != '\0') {
        return manager->config->tools.web.search.api_key;
    }
    const char* env_key = getenv("PRIMAGEN_TOOLS_WEB_SEARCH_API_KEY");
    if (env_key && env_key[0] != '\0') return env_key;
    return getenv("BRAVE_API_KEY");
}

static Error web_search_impl(void* user_data, const char* args_json, String* result) {
    WebToolContext* ctx = (WebToolContext*) user_data;
    PluginManager* manager = ctx ? ctx->manager : NULL;
    if (!get_search_enabled(manager)) {
        return error_new(ERR_INVALID_PARAM, "web_search is disabled");
    }

    cJSON* json = cJSON_Parse(args_json);
    if (!json) return error_new(ERR_JSON, "Invalid JSON arguments");

    const char* query = json_get_str(json, "query");
    int count = json_get_int(json, "count", 5);
    if (count < 1) count = 1;
    if (count > 10) count = 10;
    if (!query) {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Missing 'query' argument");
    }

    const char* api_key = get_search_api_key(manager);
    if (!api_key || api_key[0] == '\0') {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Search API key not set");
    }

    char encoded_query[2048] = {0};
    size_t qlen = strlen(query);
    size_t eidx = 0;
    for (size_t i = 0; i < qlen && eidx < sizeof(encoded_query) - 4; i++) {
        unsigned char ch = (unsigned char) query[i];
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded_query[eidx++] = (char) ch;
        } else {
            snprintf(encoded_query + eidx, 4, "%%%02X", ch);
            eidx += 3;
        }
    }

    char url[1024];
    snprintf(url, sizeof(url), "https://api.search.brave.com/res/v1/web/search?q=%s&count=%d", encoded_query, count);

    HttpChunk chunk = {0};
    chunk.memory = malloc(1);
    if (!chunk.memory) {
        cJSON_Delete(json);
        return error_new(ERR_MEMORY, "Memory allocation failed");
    }
    chunk.memory[0] = '\0';

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    struct mg_connection* c = mg_http_connect(&mgr, url, http_collect, &chunk);
    if (!c) {
        mg_mgr_free(&mgr);
        free(chunk.memory);
        cJSON_Delete(json);
        return error_new(ERR_NETWORK, "Failed to connect to Search provider");
    }

    struct mg_str host = mg_url_host(url);
    mg_printf(c,
              "GET %s HTTP/1.0\r\n"
              "Host: %.*s\r\n"
              "Accept: application/json\r\n"
              "X-Subscription-Token: %s\r\n"
              "\r\n",
              mg_url_uri(url),
              (int) host.len, host.buf,
              api_key);

    int waited_ms = 0;
    while (!chunk.done && waited_ms < WEB_REQUEST_TIMEOUT_MS) {
        mg_mgr_poll(&mgr, 1000);
        waited_ms += 1000;
    }
    mg_mgr_free(&mgr);
    if (!chunk.done) {
        free(chunk.memory);
        cJSON_Delete(json);
        return error_new(ERR_NETWORK, "Request timeout");
    }

    if (chunk.size == 0) {
        free(chunk.memory);
        cJSON_Delete(json);
        return error_new(ERR_NETWORK, "Empty response from Search provider");
    }

    cJSON* resp = cJSON_Parse(chunk.memory);
    free(chunk.memory);
    if (!resp) {
        cJSON_Delete(json);
        return error_new(ERR_JSON, "Failed to parse search response");
    }

    cJSON* web = cJSON_GetObjectItem(resp, "web");
    cJSON* items = web ? cJSON_GetObjectItem(web, "results") : NULL;

    *result = string_new("");
    char line[1024];
    snprintf(line, sizeof(line), "Results for: %s\n", query);
    string_append(result, line);

    int i = 1;
    if (cJSON_IsArray(items)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, items) {
            cJSON* title = cJSON_GetObjectItem(item, "title");
            cJSON* item_url = cJSON_GetObjectItem(item, "url");
            cJSON* desc = cJSON_GetObjectItem(item, "description");
            snprintf(line, sizeof(line), "%d. %s\n   %s\n", i++,
                     (cJSON_IsString(title) && title->valuestring) ? title->valuestring : "",
                     (cJSON_IsString(item_url) && item_url->valuestring) ? item_url->valuestring : "");
            string_append(result, line);
            if (cJSON_IsString(desc) && desc->valuestring) {
                string_append(result, "   ");
                string_append(result, desc->valuestring);
                string_append(result, "\n");
            }
        }
    }

    if (i == 1) {
        string_free(result);
        *result = string_new("No results found.");
    }

    cJSON_Delete(resp);
    cJSON_Delete(json);
    return error_new(ERR_NONE, "");
}

static Error web_fetch_impl(void* user_data, const char* args_json, String* result) {
    (void) user_data;
    cJSON* json = cJSON_Parse(args_json);
    if (!json) return error_new(ERR_JSON, "Invalid JSON arguments");

    const char* url = json_get_str(json, "url");
    if (!url) {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Missing 'url' argument");
    }

    HttpChunk chunk = {0};
    chunk.memory = malloc(1);
    if (!chunk.memory) {
        cJSON_Delete(json);
        return error_new(ERR_MEMORY, "Memory allocation failed");
    }
    chunk.memory[0] = '\0';

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    struct mg_connection* c = mg_http_connect(&mgr, url, http_collect, &chunk);
    if (!c) {
        mg_mgr_free(&mgr);
        free(chunk.memory);
        cJSON_Delete(json);
        return error_new(ERR_NETWORK, "Failed to connect to URL");
    }

    struct mg_str host = mg_url_host(url);
    mg_printf(c,
              "GET %s HTTP/1.0\r\n"
              "Host: %.*s\r\n"
              "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 14_7_2) AppleWebKit/537.36\r\n"
              "\r\n",
              mg_url_uri(url),
              (int) host.len, host.buf);

    int waited_ms = 0;
    while (!chunk.done && waited_ms < WEB_REQUEST_TIMEOUT_MS) {
        mg_mgr_poll(&mgr, 1000);
        waited_ms += 1000;
    }
    mg_mgr_free(&mgr);
    if (!chunk.done) {
        free(chunk.memory);
        cJSON_Delete(json);
        return error_new(ERR_NETWORK, "Request timeout");
    }

    if (chunk.size == 0) {
        free(chunk.memory);
        cJSON_Delete(json);
        return error_new(ERR_NETWORK, "Empty response or network error");
    }

    char* text = malloc(chunk.size + 1);
    if (!text) {
        free(chunk.memory);
        cJSON_Delete(json);
        return error_new(ERR_MEMORY, "Out of memory");
    }

    strip_tags(chunk.memory, text);
    free(chunk.memory);
    *result = string_new(text);
    free(text);
    cJSON_Delete(json);
    return error_new(ERR_NONE, "");
}

PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context) {
    (void) context;
    if (!manager) return -1;

    WebToolContext* search_ctx = malloc(sizeof(WebToolContext));
    WebToolContext* fetch_ctx = malloc(sizeof(WebToolContext));
    if (!search_ctx || !fetch_ctx) {
        free(search_ctx);
        free(fetch_ctx);
        return -1;
    }
    search_ctx->manager = manager;
    fetch_ctx->manager = manager;

    int ret1 = plugin_register_tool(
        manager, NULL, "web_search", "Search the web",
        "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"count\":{\"type\":\"integer\"}},\"required\":[\"query\"]}",
        web_search_impl, search_ctx);
    int ret2 = plugin_register_tool(
        manager, NULL, "web_fetch", "Fetch URL content",
        "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}",
        web_fetch_impl, fetch_ctx);

    if (ret1 != 0 || ret2 != 0) {
        if (ret1 != 0) free(search_ctx);
        if (ret2 != 0) free(fetch_ctx);
        return -1;
    }
    log_info("[Plugin:web_tools] Registered web_search and web_fetch");
    return 0;
}

PLUGIN_EXPORT int plugin_cleanup(void) {
    return 0;
}

static PluginInfo g_plugin_info = {
    .version = 1,
    .type = PLUGIN_TOOL,
    .name = "web_tools",
    .description = "Web search and fetch tools",
    .plugin_id = "web_tools"
};

PLUGIN_EXPORT PluginInfo* plugin_get_info(void) {
    return &g_plugin_info;
}
