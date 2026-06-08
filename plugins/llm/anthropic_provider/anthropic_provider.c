#include "providers/llm_provider.h"
#include "include/plugin.h"
#include "plugin/plugin_manager.h"
#include "include/logger.h"
#include "include/common.h"
#include "vendor/cJSON/cJSON.h"
#include "vendor/mongoose/mongoose.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

// =============================================================================
// Internal Types
// =============================================================================

typedef struct {
    char *memory;
    size_t size;
    size_t capacity;
    bool done;
    char last_error[256];
    int http_status;
} MemoryStruct;

typedef struct {
    Error error;
    String content;
    ToolCall* tool_calls;
    size_t tool_calls_count;
    FinishReason finish_reason;
    int usage_tokens;
} ParsedResponse;

typedef struct AsyncRequestContext {
    MemoryStruct chunk;
    LLMAsyncCallback callback;
    void* user_data;
    struct AnthropicAsyncManager* manager;
    uint64_t start_ms;
    uint64_t timeout_ms;
    bool callback_sent;
    struct AsyncRequestContext* next_active;
} AsyncRequestContext;

typedef struct AsyncQueueItem {
    char* url;
    char* json_str;
    char* api_key;
    AsyncRequestContext* ctx;
    Config* config;
    bool skip_tls_verify;
    struct AsyncQueueItem* next;
} AsyncQueueItem;

typedef struct AnthropicAsyncManager {
    pthread_t thread;
    bool running;
    struct mg_mgr mgr;
    AsyncQueueItem* queue_head;
    AsyncQueueItem* queue_tail;
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;
    bool dns_configured;
    AsyncRequestContext* active_head;
} AnthropicAsyncManager;

// =============================================================================
// Forward Declarations
// =============================================================================

static AnthropicAsyncManager* anthropic_async_manager_new(void);
static void anthropic_async_manager_free(AnthropicAsyncManager* manager);
static void anthropic_async_manager_start(AnthropicAsyncManager* manager);
static void anthropic_async_manager_stop(AnthropicAsyncManager* manager);
static Error anthropic_provider_call_extended(LLMProvider* provider, const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, LLMResponse* llm_response);
static void anthropic_async_ev_handler(struct mg_connection *c, int ev, void *ev_data);

// =============================================================================
// Helpers
// =============================================================================

static const char* get_anthropic_config_string(Config* config, const char* key, const char* default_val) {
    if (!config) return default_val;
    PluginConfig* pc = config_get_plugin_config(config, "anthropic_provider");
    if (!pc || !pc->config) return default_val;
    cJSON* item = cJSON_GetObjectItem(pc->config, key);
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0])
        return item->valuestring;
    return default_val;
}

static double get_anthropic_config_double(Config* config, const char* key, double default_val) {
    if (!config) return default_val;
    PluginConfig* pc = config_get_plugin_config(config, "anthropic_provider");
    if (!pc || !pc->config) return default_val;
    cJSON* item = cJSON_GetObjectItem(pc->config, key);
    if (cJSON_IsNumber(item)) return item->valuedouble;
    return default_val;
}

static int get_anthropic_config_int(Config* config, const char* key, int default_val) {
    if (!config) return default_val;
    PluginConfig* pc = config_get_plugin_config(config, "anthropic_provider");
    if (!pc || !pc->config) return default_val;
    cJSON* item = cJSON_GetObjectItem(pc->config, key);
    if (cJSON_IsNumber(item)) return item->valueint;
    return default_val;
}

static const char* get_anthropic_api_key(Config* config) {
    const char* key = get_anthropic_config_string(config, "apiKey", NULL);
    if (key) return key;
    const char* env_key = getenv("ANTHROPIC_API_KEY");
    return env_key ? env_key : "";
}

static const char* get_anthropic_model(Config* config) {
    return get_anthropic_config_string(config, "model", "claude-sonnet-4-20250514");
}

static const char* get_anthropic_api_base(Config* config) {
    return get_anthropic_config_string(config, "apiBase", "https://api.anthropic.com");
}

static double get_anthropic_temperature(Config* config) {
    return get_anthropic_config_double(config, "temperature", 0.1);
}

static int get_anthropic_max_tokens(Config* config) {
    return get_anthropic_config_int(config, "max_tokens", 4096);
}

static bool should_skip_tls_verification(void) {
    const char* val = getenv("PRIMAGEN_TLS_SKIP_VERIFY");
    if (!val) return false;
    return strcmp(val, "1") == 0 || strcmp(val, "true") == 0;
}

static void configure_mgr_dns(struct mg_mgr* mgr, const Config* config) {
    if (!mgr || !config) return;
    mgr->use_system_resolver = config->dns.use_system_resolver ? true : false;
    mgr->dns4.url = config->dns.dns4_url ? config->dns.dns4_url : "udp://8.8.8.8:53";
    mgr->dns6.url = config->dns.dns6_url ? config->dns.dns6_url : NULL;
    mgr->dnstimeout = (config->dns.dns_timeout_ms > 0) ? config->dns.dns_timeout_ms : 5000;
}

static char* build_anthropic_api_url(Config* config) {
    const char* api_base = get_anthropic_api_base(config);
    size_t url_len = strlen(api_base) + strlen("/v1/messages") + 2;
    char* url = malloc(url_len);
    if (!url) return NULL;
    snprintf(url, url_len, "%s%s/v1/messages", api_base, api_base[strlen(api_base)-1] == '/' ? "" : "/");
    return url;
}

static FinishReason parse_anthropic_stop_reason(const char* reason) {
    if (!reason) return FINISH_REASON_NONE;
    if (strcmp(reason, "end_turn") == 0) return FINISH_REASON_STOP;
    if (strcmp(reason, "max_tokens") == 0) return FINISH_REASON_LENGTH;
    if (strcmp(reason, "stop_sequence") == 0) return FINISH_REASON_STOP;
    if (strcmp(reason, "tool_use") == 0) return FINISH_REASON_TOOL_CALLS;
    return FINISH_REASON_ERROR;
}

static cJSON* build_anthropic_messages(Session* session, Config* config) {
    cJSON *messages = cJSON_CreateArray();
    if (!session) return messages;

    size_t start_idx = 0;
    size_t max_history = config && config->agent.memory_window > 0 ? (size_t)config->agent.memory_window : 30;
    if (session->messages.count > max_history)
        start_idx = session->messages.count - max_history;

    for (size_t i = start_idx; i < session->messages.count; i++) {
        Message* msg = *(Message**)dynamic_array_get(&session->messages, i);
        cJSON *json_msg = cJSON_CreateObject();

        if (msg->role == ROLE_USER) {
            cJSON_AddStringToObject(json_msg, "role", "user");
            cJSON *content_arr = cJSON_CreateArray();
            cJSON *text_block = cJSON_CreateObject();
            cJSON_AddStringToObject(text_block, "type", "text");
            cJSON_AddStringToObject(text_block, "text", msg->content.data);
            cJSON_AddItemToArray(content_arr, text_block);
            cJSON_AddItemToObject(json_msg, "content", content_arr);
        } else if (msg->role == ROLE_ASSISTANT) {
            cJSON_AddStringToObject(json_msg, "role", "assistant");
            cJSON *content_arr = cJSON_CreateArray();
            if (msg->content.len > 0) {
                cJSON *text_block = cJSON_CreateObject();
                cJSON_AddStringToObject(text_block, "type", "text");
                cJSON_AddStringToObject(text_block, "text", msg->content.data);
                cJSON_AddItemToArray(content_arr, text_block);
            }
            for (size_t j = 0; j < msg->tool_calls_count; j++) {
                cJSON *tool_block = cJSON_CreateObject();
                cJSON_AddStringToObject(tool_block, "type", "tool_use");
                cJSON_AddStringToObject(tool_block, "id", msg->tool_calls[j].id.data);
                cJSON_AddStringToObject(tool_block, "name", msg->tool_calls[j].name.data);
                cJSON *input = cJSON_Parse(msg->tool_calls[j].arguments.data);
                if (input) cJSON_AddItemToObject(tool_block, "input", input);
                else cJSON_AddItemToObject(tool_block, "input", cJSON_CreateObject());
                cJSON_AddItemToArray(content_arr, tool_block);
            }
            cJSON_AddItemToObject(json_msg, "content", content_arr);
        } else if (msg->role == ROLE_TOOL) {
            cJSON_AddStringToObject(json_msg, "role", "user");
            cJSON *content_arr = cJSON_CreateArray();
            cJSON *result_block = cJSON_CreateObject();
            cJSON_AddStringToObject(result_block, "type", "tool_result");
            cJSON_AddStringToObject(result_block, "tool_use_id", msg->tool_call_id.data);
            cJSON_AddStringToObject(result_block, "content", msg->content.data);
            cJSON_AddItemToArray(content_arr, result_block);
            cJSON_AddItemToObject(json_msg, "content", content_arr);
        }
        cJSON_AddItemToArray(messages, json_msg);
    }
    return messages;
}

static void add_anthropic_tools(cJSON* root, ToolRegistry* tools) {
    if (!tools || tools->count == 0) return;
    cJSON *tools_json = cJSON_CreateArray();
    for (size_t i = 0; i < tools->count; i++) {
        cJSON *tool_item = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_item, "name", tools->tools[i].def.name.data);
        cJSON_AddStringToObject(tool_item, "description", tools->tools[i].def.description.data);
        cJSON *schema = cJSON_Parse(tools->tools[i].def.parameters.data);
        if (schema) cJSON_AddItemToObject(tool_item, "input_schema", schema);
        else cJSON_AddItemToObject(tool_item, "input_schema", cJSON_CreateObject());
        cJSON_AddItemToArray(tools_json, tool_item);
    }
    cJSON_AddItemToObject(root, "tools", tools_json);
}

static char* build_anthropic_request_json(const char* system_prompt, Session* session, ToolRegistry* tools, Config* config) {
    cJSON *root = cJSON_CreateObject();
    const char* model = get_anthropic_model(config);
    cJSON_AddStringToObject(root, "model", model);
    int max_tokens = get_anthropic_max_tokens(config);
    cJSON_AddNumberToObject(root, "max_tokens", max_tokens);
    cJSON_AddNumberToObject(root, "temperature", get_anthropic_temperature(config));

    if (system_prompt && strlen(system_prompt) > 0) {
        cJSON_AddStringToObject(root, "system", system_prompt);
    }

    cJSON_AddItemToObject(root, "messages", build_anthropic_messages(session, config));
    add_anthropic_tools(root, tools);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

static ParsedResponse parse_anthropic_response(const char* json_data, int http_status, const char* last_error) {
    ParsedResponse result = {0};
    result.content = string_new("");
    result.finish_reason = FINISH_REASON_NONE;

    if (!json_data || strlen(json_data) == 0) {
        char errbuf[320];
        snprintf(errbuf, sizeof(errbuf), "Empty response from Anthropic (status=%d, %s)", http_status, last_error[0] ? last_error : "no payload");
        result.error = error_new(ERR_NETWORK, errbuf);
        return result;
    }

    cJSON *json = cJSON_Parse(json_data);
    if (!json) { result.error = error_new(ERR_JSON, "Failed to parse Anthropic response"); return result; }

    cJSON *error_obj = cJSON_GetObjectItem(json, "error");
    if (error_obj) {
        cJSON *msg_item = cJSON_GetObjectItem(error_obj, "message");
        result.error = error_new(ERR_NETWORK, msg_item ? msg_item->valuestring : "Unknown Anthropic API error");
        cJSON_Delete(json);
        return result;
    }

    cJSON *stop_reason = cJSON_GetObjectItem(json, "stop_reason");
    if (stop_reason && cJSON_IsString(stop_reason))
        result.finish_reason = parse_anthropic_stop_reason(stop_reason->valuestring);

    cJSON *usage = cJSON_GetObjectItem(json, "usage");
    if (usage) {
        cJSON *input_tokens = cJSON_GetObjectItem(usage, "input_tokens");
        cJSON *output_tokens = cJSON_GetObjectItem(usage, "output_tokens");
        result.usage_tokens = 0;
        if (input_tokens && cJSON_IsNumber(input_tokens)) result.usage_tokens += input_tokens->valueint;
        if (output_tokens && cJSON_IsNumber(output_tokens)) result.usage_tokens += output_tokens->valueint;
    }

    cJSON *content = cJSON_GetObjectItem(json, "content");
    if (cJSON_IsArray(content)) {
        size_t tool_count = 0;
        int arr_size = cJSON_GetArraySize(content);
        for (int i = 0; i < arr_size; i++) {
            cJSON *block = cJSON_GetArrayItem(content, i);
            cJSON *type = cJSON_GetObjectItem(block, "type");
            if (type && cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        const char* trimmed = text->valuestring;
                        while (*trimmed == '\n' || *trimmed == '\r') trimmed++;
                        string_append(&result.content, trimmed);
                    }
                } else if (strcmp(type->valuestring, "tool_use") == 0) {
                    tool_count++;
                }
            }
        }

        if (tool_count > 0) {
            result.tool_calls = calloc(tool_count, sizeof(ToolCall));
            size_t idx = 0;
            for (int i = 0; i < arr_size; i++) {
                cJSON *block = cJSON_GetArrayItem(content, i);
                cJSON *type = cJSON_GetObjectItem(block, "type");
                if (type && cJSON_IsString(type) && strcmp(type->valuestring, "tool_use") == 0) {
                    cJSON *id = cJSON_GetObjectItem(block, "id");
                    cJSON *name = cJSON_GetObjectItem(block, "name");
                    cJSON *input = cJSON_GetObjectItem(block, "input");
                    result.tool_calls[idx].id = string_new(id && cJSON_IsString(id) ? id->valuestring : "");
                    result.tool_calls[idx].name = string_new(name && cJSON_IsString(name) ? name->valuestring : "");
                    if (input) {
                        char *args_str = cJSON_PrintUnformatted(input);
                        result.tool_calls[idx].arguments = string_new(args_str ? args_str : "");
                        free(args_str);
                    } else {
                        result.tool_calls[idx].arguments = string_new("{}");
                    }
                    idx++;
                }
            }
            result.tool_calls_count = idx;
        }
    }

    cJSON_Delete(json);
    return result;
}

static void free_parsed_response(ParsedResponse* r) {
    string_free(&r->content);
    for (size_t i = 0; i < r->tool_calls_count; i++) {
        string_free(&r->tool_calls[i].id);
        string_free(&r->tool_calls[i].name);
        string_free(&r->tool_calls[i].arguments);
    }
    free(r->tool_calls);
}

// =============================================================================
// Sync HTTP Event Handler
// =============================================================================

static void sync_ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    MemoryStruct *ms = (MemoryStruct *)c->fn_data;
    if (ev == MG_EV_HTTP_HDRS) {
        ms->http_status = mg_http_status((struct mg_http_message *)ev_data);
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        ms->http_status = mg_http_status(hm);
        size_t new_size = ms->size + hm->body.len;
        if (new_size + 1 > ms->capacity) {
            size_t new_cap = ms->capacity * 2;
            while (new_size + 1 > new_cap) new_cap *= 2;
            char *new_mem = realloc(ms->memory, new_cap);
            if (!new_mem) { snprintf(ms->last_error, sizeof(ms->last_error), "OOM"); ms->done = true; c->is_closing = 1; return; }
            ms->memory = new_mem; ms->capacity = new_cap;
        }
        memcpy(ms->memory + ms->size, hm->body.buf, hm->body.len);
        ms->size = new_size; ms->memory[ms->size] = '\0';
        c->is_closing = 1; ms->done = true;
    } else if (ev == MG_EV_CLOSE) {
        if (ms->size == 0 && !ms->done) ms->done = true;
    } else if (ev == MG_EV_ERROR) {
        snprintf(ms->last_error, sizeof(ms->last_error), "%s", ev_data ? (const char*)ev_data : "unknown");
        ms->done = true;
    }
}

// =============================================================================
// Async Worker
// =============================================================================

static void anthropic_async_manager_add_active(AnthropicAsyncManager* manager, AsyncRequestContext* ctx) {
    if (!manager || !ctx) return;
    ctx->next_active = manager->active_head;
    manager->active_head = ctx;
}

static void anthropic_async_manager_remove_active(AnthropicAsyncManager* manager, AsyncRequestContext* ctx) {
    if (!manager || !ctx) return;
    AsyncRequestContext** cur = &manager->active_head;
    while (*cur) {
        if (*cur == ctx) {
            *cur = ctx->next_active;
            ctx->next_active = NULL;
            return;
        }
        cur = &(*cur)->next_active;
    }
}

static void anthropic_async_context_callback_once(AsyncRequestContext* ctx, Error err, const char* response, ToolCall* tool_calls, size_t tool_calls_count) {
    if (!ctx || ctx->callback_sent) return;
    ctx->callback_sent = true;
    if (ctx->callback) {
        ctx->callback(err, response, tool_calls, tool_calls_count, ctx->user_data);
    }
}

static void anthropic_async_manager_check_timeouts(AnthropicAsyncManager* manager) {
    if (!manager) return;
    uint64_t now = mg_millis();
    struct mg_connection* c = manager->mgr.conns;
    while (c) {
        struct mg_connection* next = c->next;
        AsyncRequestContext* ctx = (AsyncRequestContext*)c->fn_data;
        if (ctx && ctx->timeout_ms > 0 && now - ctx->start_ms >= ctx->timeout_ms) {
            snprintf(ctx->chunk.last_error, sizeof(ctx->chunk.last_error), "Anthropic request timed out");
            ctx->chunk.done = true;
            c->is_closing = 1;
        }
        c = next;
    }
}

static AnthropicAsyncManager* anthropic_async_manager_new(void) {
    AnthropicAsyncManager* manager = calloc(1, sizeof(AnthropicAsyncManager));
    if (!manager) return NULL;
    mg_mgr_init(&manager->mgr);
    manager->running = false;
    pthread_mutex_init(&manager->queue_mutex, NULL);
    pthread_cond_init(&manager->queue_cond, NULL);
    return manager;
}

static void anthropic_async_manager_free(AnthropicAsyncManager* manager) {
    if (!manager) return;
    if (manager->running) anthropic_async_manager_stop(manager);
    mg_mgr_free(&manager->mgr);
    pthread_mutex_destroy(&manager->queue_mutex);
    pthread_cond_destroy(&manager->queue_cond);
    free(manager);
}

static void async_worker_process_item(AsyncQueueItem* item, AnthropicAsyncManager* manager) {
    if (!item || !item->ctx) return;
    if (!manager->dns_configured && item->config) {
        configure_mgr_dns(&manager->mgr, item->config);
        manager->dns_configured = true;
    }
    struct mg_connection *c = mg_http_connect(&manager->mgr, item->url, anthropic_async_ev_handler, item->ctx);
    if (!c) {
        if (item->ctx->callback) item->ctx->callback(error_new(ERR_NETWORK, "Failed to connect to Anthropic"), NULL, NULL, 0, item->ctx->user_data);
        free(item->ctx->chunk.memory); free(item->ctx);
    } else {
        anthropic_async_manager_add_active(manager, item->ctx);
        struct mg_str host = mg_url_host(item->url);
        struct mg_tls_opts opts = {
            .ca = mg_str(""),
            .cert = {0},
            .key = {0},
            .name = host,
            .skip_verification = item->skip_tls_verify ? 1 : 0,
        };
        if (mg_url_is_ssl(item->url)) mg_tls_init(c, &opts);
        mg_printf(c, "POST %s HTTP/1.0\r\nHost: %.*s\r\nContent-Type: application/json\r\nx-api-key: %s\r\nanthropic-version: 2023-06-01\r\nContent-Length: %d\r\n\r\n%s",
            mg_url_uri(item->url), (int)host.len, host.buf, item->api_key, (int)strlen(item->json_str), item->json_str);
    }

    free(item->url); free(item->json_str); free(item->api_key);
    free(item);
}

static AsyncQueueItem* async_worker_dequeue_all(AnthropicAsyncManager* manager) {
    pthread_mutex_lock(&manager->queue_mutex);
    AsyncQueueItem* items = manager->queue_head;
    manager->queue_head = NULL; manager->queue_tail = NULL;
    pthread_mutex_unlock(&manager->queue_mutex);
    return items;
}

static void* anthropic_async_worker(void* arg) {
    AnthropicAsyncManager* manager = (AnthropicAsyncManager*)arg;
    if (!manager) return NULL;
    while (manager->running) {
        AsyncQueueItem* items = async_worker_dequeue_all(manager);
        while (items) { AsyncQueueItem* next = items->next; async_worker_process_item(items, manager); items = next; }
        mg_mgr_poll(&manager->mgr, 10);
        anthropic_async_manager_check_timeouts(manager);
        pthread_mutex_lock(&manager->queue_mutex);
        if (!manager->queue_head && manager->running) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 5000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&manager->queue_cond, &manager->queue_mutex, &ts);
        }
        pthread_mutex_unlock(&manager->queue_mutex);
    }
    AsyncQueueItem* remaining = async_worker_dequeue_all(manager);
    while (remaining) {
        AsyncQueueItem* next = remaining->next;
        if (remaining->ctx->callback) remaining->ctx->callback(error_new(ERR_NETWORK, "Shutting down"), NULL, NULL, 0, remaining->ctx->user_data);
        free(remaining->ctx->chunk.memory); free(remaining->ctx);
        free(remaining->url); free(remaining->json_str); free(remaining->api_key);
        free(remaining); remaining = next;
    }
    mg_mgr_poll(&manager->mgr, 0);
    AsyncRequestContext* active = manager->active_head;
    manager->active_head = NULL;
    while (active) {
        AsyncRequestContext* next = active->next_active;
        anthropic_async_context_callback_once(active, error_new(ERR_NETWORK, "Shutting down"), NULL, NULL, 0);
        free(active->chunk.memory);
        free(active);
        active = next;
    }
    return NULL;
}

static void anthropic_async_manager_start(AnthropicAsyncManager* manager) {
    if (!manager || manager->running) return;
    manager->running = true;
    pthread_create(&manager->thread, NULL, anthropic_async_worker, manager);
}

static void anthropic_async_manager_stop(AnthropicAsyncManager* manager) {
    if (!manager || !manager->running) return;
    manager->running = false;
    pthread_mutex_lock(&manager->queue_mutex); pthread_cond_signal(&manager->queue_cond); pthread_mutex_unlock(&manager->queue_mutex);
    pthread_join(manager->thread, NULL);
}

// =============================================================================
// Async Event Handler
// =============================================================================

static void anthropic_async_ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    AsyncRequestContext *ctx = (AsyncRequestContext *)c->fn_data;
    MemoryStruct *ms = &ctx->chunk;

    if (ev == MG_EV_HTTP_HDRS) {
        ms->http_status = mg_http_status((struct mg_http_message *)ev_data);
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        ms->http_status = mg_http_status(hm);
        size_t new_size = ms->size + hm->body.len;
        if (new_size + 1 > ms->capacity) {
            size_t new_cap = ms->capacity * 2;
            while (new_size + 1 > new_cap) new_cap *= 2;
            char *new_mem = realloc(ms->memory, new_cap);
            if (!new_mem) { snprintf(ms->last_error, sizeof(ms->last_error), "OOM"); ms->done = true; c->is_closing = 1; return; }
            ms->memory = new_mem; ms->capacity = new_cap;
        }
        memcpy(ms->memory + ms->size, hm->body.buf, hm->body.len);
        ms->size = new_size; ms->memory[ms->size] = '\0';
        c->is_closing = 1; ms->done = true;
    } else if (ev == MG_EV_CLOSE) {
        if (ms->size == 0 && !ms->done) ms->done = true;
        ParsedResponse parsed = parse_anthropic_response(ms->size > 0 ? ms->memory : NULL, ms->http_status, ms->last_error[0] ? ms->last_error : "no payload");
        if (ctx->manager) {
            anthropic_async_manager_remove_active(ctx->manager, ctx);
        }
        if (ctx->callback) {
            char* response_copy = parsed.content.data ? strdup(parsed.content.data) : NULL;
            ToolCall* tool_calls_copy = NULL;
            size_t tool_calls_count_copy = parsed.tool_calls_count;
            if (parsed.tool_calls_count > 0 && parsed.tool_calls) {
                tool_calls_copy = calloc(parsed.tool_calls_count, sizeof(ToolCall));
                if (tool_calls_copy) {
                    for (size_t i = 0; i < parsed.tool_calls_count; i++) {
                        tool_calls_copy[i].id = string_copy(&parsed.tool_calls[i].id);
                        tool_calls_copy[i].name = string_copy(&parsed.tool_calls[i].name);
                        tool_calls_copy[i].arguments = string_copy(&parsed.tool_calls[i].arguments);
                    }
                } else tool_calls_count_copy = 0;
            }
            anthropic_async_context_callback_once(ctx, parsed.error, response_copy, tool_calls_copy, tool_calls_count_copy);
            free(response_copy);
            if (tool_calls_copy) { for (size_t i = 0; i < tool_calls_count_copy; i++) { string_free(&tool_calls_copy[i].id); string_free(&tool_calls_copy[i].name); string_free(&tool_calls_copy[i].arguments); } free(tool_calls_copy); }
        }
        free_parsed_response(&parsed);
        free(ms->memory); free(ctx);
    } else if (ev == MG_EV_ERROR) {
        snprintf(ms->last_error, sizeof(ms->last_error), "%s", ev_data ? (const char*)ev_data : "unknown");
        ms->done = true;
    }
}

// =============================================================================
// Provider Interface Implementation
// =============================================================================

static Error anthropic_provider_init(LLMProvider* provider, Config* config) {
    (void)config;
    if (!provider) return error_new(ERR_INVALID_PARAM, "provider is NULL");
    AnthropicAsyncManager* manager = anthropic_async_manager_new();
    if (!manager) return error_new(ERR_NETWORK, "Failed to create async manager");
    anthropic_async_manager_start(manager);
    provider->state = manager;
    provider->initialized = true;
    return error_new(ERR_NONE, "");
}

static void anthropic_provider_shutdown(LLMProvider* provider) {
    if (!provider) return;
    AnthropicAsyncManager* manager = (AnthropicAsyncManager*)provider->state;
    if (manager) { anthropic_async_manager_stop(manager); anthropic_async_manager_free(manager); provider->state = NULL; }
    provider->initialized = false;
}

static Error anthropic_provider_call(LLMProvider* provider, const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, String* response, ToolCall** tool_calls, size_t* tool_calls_count) {
    LLMResponse llm_response;
    Error err = anthropic_provider_call_extended(provider, system_prompt, session, tools, config, &llm_response);
    if (err.code != ERR_NONE) return err;
    *response = llm_response.content;
    *tool_calls = llm_response.tool_calls;
    *tool_calls_count = llm_response.tool_calls_count;
    return error_new(ERR_NONE, "");
}

static Error anthropic_provider_call_extended(LLMProvider* provider, const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, LLMResponse* llm_response) {
    (void)provider;
    if (!llm_response) return error_new(ERR_INVALID_PARAM, "llm_response is NULL");

    llm_response->content = string_new("");
    llm_response->tool_calls = NULL; llm_response->tool_calls_count = 0;
    llm_response->finish_reason = FINISH_REASON_NONE; llm_response->usage_tokens = 0;

    struct mg_mgr mgr;
    MemoryStruct chunk = {0};
    chunk.capacity = 4096; chunk.memory = malloc(chunk.capacity);
    if (!chunk.memory) return error_new(ERR_MEMORY, "OOM allocating response buffer");
    chunk.memory[0] = '\0';

    mg_mgr_init(&mgr);
    configure_mgr_dns(&mgr, config);

    const char* api_key = get_anthropic_api_key(config);
    if (strlen(api_key) == 0) { mg_mgr_free(&mgr); free(chunk.memory); return error_new(ERR_INVALID_PARAM, "Anthropic API Key not set (config or ANTHROPIC_API_KEY env)"); }

    char *json_str = build_anthropic_request_json(system_prompt, session, tools, config);
    char* url = build_anthropic_api_url(config);
    if (!url) { free(json_str); mg_mgr_free(&mgr); free(chunk.memory); return error_new(ERR_NETWORK, "OOM building URL"); }

    struct mg_connection *c = mg_http_connect(&mgr, url, sync_ev_handler, &chunk);
    if (!c) { free(url); free(json_str); mg_mgr_free(&mgr); free(chunk.memory); return error_new(ERR_NETWORK, "Failed to connect to Anthropic"); }

    struct mg_str host = mg_url_host(url);
    struct mg_tls_opts opts = {
        .ca = mg_str(""),
        .cert = {0},
        .key = {0},
        .name = host,
        .skip_verification = should_skip_tls_verification() ? 1 : 0,
    };
    if (mg_url_is_ssl(url)) mg_tls_init(c, &opts);

    mg_printf(c, "POST %s HTTP/1.0\r\nHost: %.*s\r\nContent-Type: application/json\r\nx-api-key: %s\r\nanthropic-version: 2023-06-01\r\nContent-Length: %d\r\n\r\n%s",
        mg_url_uri(url), (int)host.len, host.buf, api_key, (int)strlen(json_str), json_str);

    { uint64_t start_ms = mg_millis(); while (!chunk.done && (mg_millis() - start_ms) < 120000) mg_mgr_poll(&mgr, 1000); }

    free(url); free(json_str); mg_mgr_free(&mgr);
    ParsedResponse parsed = parse_anthropic_response(chunk.memory, chunk.http_status, chunk.last_error);
    free(chunk.memory);

    if (parsed.error.code != ERR_NONE) { free_parsed_response(&parsed); return parsed.error; }
    llm_response->content = parsed.content;
    llm_response->tool_calls = parsed.tool_calls;
    llm_response->tool_calls_count = parsed.tool_calls_count;
    llm_response->finish_reason = parsed.finish_reason;
    llm_response->usage_tokens = parsed.usage_tokens;
    parsed.tool_calls = NULL; parsed.tool_calls_count = 0; memset(&parsed.content, 0, sizeof(parsed.content));
    return error_new(ERR_NONE, "");
}

static void anthropic_provider_call_async(LLMProvider* provider, const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, LLMAsyncCallback callback, void* user_data) {
    if (!provider || !provider->state) { if (callback) callback(error_new(ERR_INVALID_PARAM, "Provider not initialized"), NULL, NULL, 0, user_data); return; }
    AnthropicAsyncManager* manager = (AnthropicAsyncManager*)provider->state;
    const char* api_key = get_anthropic_api_key(config);
    if (strlen(api_key) == 0) { if (callback) callback(error_new(ERR_INVALID_PARAM, "Anthropic API Key not set"), NULL, NULL, 0, user_data); return; }

    char *json_str = build_anthropic_request_json(system_prompt, session, tools, config);
    char* url = build_anthropic_api_url(config);
    if (!url || !json_str) { free(json_str); free(url); if (callback) callback(error_new(ERR_NETWORK, "OOM"), NULL, NULL, 0, user_data); return; }

    AsyncRequestContext *ctx = calloc(1, sizeof(AsyncRequestContext));
    if (!ctx) { free(url); free(json_str); if (callback) callback(error_new(ERR_MEMORY, "OOM allocating request context"), NULL, NULL, 0, user_data); return; }
    ctx->callback = callback; ctx->user_data = user_data;
    ctx->manager = manager;
    ctx->chunk.capacity = 4096; ctx->chunk.memory = malloc(ctx->chunk.capacity);
    if (!ctx->chunk.memory) { free(ctx); free(url); free(json_str); if (callback) callback(error_new(ERR_MEMORY, "OOM allocating response buffer"), NULL, NULL, 0, user_data); return; }
    ctx->chunk.memory[0] = '\0'; ctx->start_ms = mg_millis(); ctx->timeout_ms = 120000;

    AsyncQueueItem* item = calloc(1, sizeof(AsyncQueueItem));
    if (!item) { free(ctx->chunk.memory); free(ctx); free(url); free(json_str); if (callback) callback(error_new(ERR_NETWORK, "OOM"), NULL, NULL, 0, user_data); return; }
    item->url = url; item->json_str = json_str; item->api_key = strdup(api_key);
    item->ctx = ctx; item->config = config; item->skip_tls_verify = should_skip_tls_verification(); item->next = NULL;

    pthread_mutex_lock(&manager->queue_mutex);
    if (manager->queue_tail) manager->queue_tail->next = item; else manager->queue_head = item;
    manager->queue_tail = item;
    pthread_cond_signal(&manager->queue_cond);
    pthread_mutex_unlock(&manager->queue_mutex);
}

static Error anthropic_provider_call_streaming(LLMProvider* provider, const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, LLMResponse* llm_response, LLMStreamOptions* options) {
    (void)provider; (void)system_prompt; (void)session; (void)tools; (void)config; (void)llm_response; (void)options;
    return error_new(ERR_INVALID_PARAM, "Streaming not yet implemented for Anthropic provider");
}

static const LLMProviderInterface ANTHROPIC_PROVIDER_INTERFACE = {
    .name = "Anthropic Compatible",
    .init = anthropic_provider_init,
    .shutdown = anthropic_provider_shutdown,
    .call = anthropic_provider_call,
    .call_extended = anthropic_provider_call_extended,
    .call_async = anthropic_provider_call_async,
    .call_streaming = anthropic_provider_call_streaming,
};

// =============================================================================
// Plugin Exports
// =============================================================================

static LLMProviderPluginDef g_provider_def = {
    .name = "anthropic_provider",
    .iface = &ANTHROPIC_PROVIDER_INTERFACE,
};

PLUGIN_EXPORT PluginInfo* plugin_get_info(void) {
    static PluginInfo info = {
        .version = 1,
        .type = PLUGIN_LLM_PROVIDER,
        .name = "anthropic_provider",
        .description = "Anthropic Claude API compatible LLM provider",
        .plugin_id = "anthropic_provider",
        .metadata = &g_provider_def,
    };
    return &info;
}

PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context) {
    int ret = plugin_register_llm_provider(manager, (LoadedPlugin*)context, "anthropic_provider", &ANTHROPIC_PROVIDER_INTERFACE);
    if (ret != 0) return -1;
    return 0;
}

PLUGIN_EXPORT int plugin_cleanup(void) {
    return 0;
}
