#include "openai_provider.h"
#include "../include/logger.h"
#include "../include/common.h"
#include "../vendor/cJSON/cJSON.h"
#include "../vendor/mongoose/mongoose.h"

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
} ParsedLLMResponse;

typedef struct AsyncRequestContext {
    MemoryStruct chunk;
    LLMAsyncCallback callback;
    void* user_data;
    struct LLMAsyncManager* manager;
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

typedef struct LLMAsyncManager {
    pthread_t thread;
    bool running;
    struct mg_mgr mgr;
    AsyncQueueItem* queue_head;
    AsyncQueueItem* queue_tail;
    pthread_mutex_t queue_mutex;
    pthread_mutex_t dns_mutex;
    pthread_cond_t queue_cond;
    bool dns_configured;
    AsyncRequestContext* active_head;
} LLMAsyncManager;

typedef struct {
    LLMResponse* llm_response;
    StreamCallback on_chunk;
    void* user_data;
    bool done;
    char last_error[256];
    String accumulated_content;
    String recv_buf;
    ToolCall* tool_calls_accum;
    size_t tool_calls_accum_count;
    size_t tool_calls_accum_capacity;
} StreamContext;

// =============================================================================
// Forward Declarations
// =============================================================================

static void stream_context_cleanup(StreamContext* ctx);

static LLMAsyncManager* llm_async_manager_new(void);
static void llm_async_manager_free(LLMAsyncManager* manager);
static void llm_async_manager_start(LLMAsyncManager* manager);
static void llm_async_manager_stop(LLMAsyncManager* manager);
static Error openai_provider_call_extended(LLMProvider* provider, const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, LLMResponse* llm_response);

// =============================================================================
// Helpers
// =============================================================================

static FinishReason parse_finish_reason(const char* reason) {
    if (!reason) return FINISH_REASON_NONE;
    if (strcmp(reason, "stop") == 0) return FINISH_REASON_STOP;
    if (strcmp(reason, "length") == 0) return FINISH_REASON_LENGTH;
    if (strcmp(reason, "tool_calls") == 0) return FINISH_REASON_TOOL_CALLS;
    if (strcmp(reason, "content_filter") == 0) return FINISH_REASON_CONTENT_FILTER;
    return FINISH_REASON_ERROR;
}

static const char* json_get_string(cJSON* obj, const char* key, const char* fallback) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && item->valuestring) return item->valuestring;
    return fallback;
}

static ProviderConfig* get_provider_config(Config* config) {
    if (!config) return NULL;
    return config_get_active_provider(config);
}

static const char* get_api_key(Config* config) {
    ProviderConfig* pc = get_provider_config(config);
    if (pc && pc->api_key && strlen(pc->api_key) > 0) {
        log_debug("[OpenAI Provider] Using API key from config");
        return pc->api_key;
    }
    const char* key = getenv("OPENAI_API_KEY");
    if (key && strlen(key) > 0) {
        log_debug("[OpenAI Provider] Using API key from OPENAI_API_KEY environment variable");
    }
    return key ? key : "";
}

static bool should_skip_tls_verification(void) {
    const char* val = getenv("PRIMAGEN_TLS_SKIP_VERIFY");
    if (!val) return false;
    if (strcmp(val, "1") == 0 || strcmp(val, "true") == 0 || strcmp(val, "yes") == 0) {
        log_warn("[OpenAI Provider] TLS certificate verification is DISABLED - this is insecure and should only be used for testing");
        return true;
    }
    return false;
}

static void configure_mgr_dns(struct mg_mgr* mgr, const Config* config) {
    if (!mgr || !config) return;
    mgr->use_system_resolver = config->dns.use_system_resolver ? true : false;
    mgr->dns4.url = config->dns.dns4_url ? config->dns.dns4_url : "udp://8.8.8.8:53";
    mgr->dns6.url = config->dns.dns6_url ? config->dns.dns6_url : NULL;
    mgr->dnstimeout = (config->dns.dns_timeout_ms > 0) ? config->dns.dns_timeout_ms : 5000;
}

static cJSON* build_messages_json(const char* system_prompt, Session* session, Config* config) {
    cJSON *messages = cJSON_CreateArray();
    if (system_prompt && strlen(system_prompt) > 0) {
        cJSON *sys_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_msg, "role", "system");
        cJSON_AddStringToObject(sys_msg, "content", system_prompt);
        cJSON_AddItemToArray(messages, sys_msg);
    }
    if (session) {
        size_t max_history = config && config->agent.memory_window > 0 ? (size_t)config->agent.memory_window : 30;
        SessionSnapshot snapshot = session_snapshot_for_context(session, max_history);
        if (snapshot.truncated) {
            cJSON *note = cJSON_CreateObject();
            cJSON_AddStringToObject(note, "role", "system");
            cJSON_AddStringToObject(note, "content", "(Note: Older conversation history has been truncated.)");
            cJSON_AddItemToArray(messages, note);
        }
        for (size_t i = 0; i < snapshot.count; i++) {
            Message* msg = snapshot.messages[i];
            cJSON *json_msg = cJSON_CreateObject();
            if (msg->role == ROLE_USER) {
                cJSON_AddStringToObject(json_msg, "role", "user");
                cJSON_AddStringToObject(json_msg, "content", msg->content.data);
            } else if (msg->role == ROLE_ASSISTANT) {
                cJSON_AddStringToObject(json_msg, "role", "assistant");
                if (msg->content.len > 0) cJSON_AddStringToObject(json_msg, "content", msg->content.data);
                else cJSON_AddNullToObject(json_msg, "content");
                if (msg->tool_calls_count > 0) {
                    cJSON *tcs = cJSON_CreateArray();
                    for (size_t j = 0; j < msg->tool_calls_count; j++) {
                        cJSON *tc = cJSON_CreateObject();
                        cJSON_AddStringToObject(tc, "id", msg->tool_calls[j].id.data);
                        cJSON_AddStringToObject(tc, "type", "function");
                        cJSON *func = cJSON_CreateObject();
                        cJSON_AddStringToObject(func, "name", msg->tool_calls[j].name.data);
                        cJSON_AddStringToObject(func, "arguments", msg->tool_calls[j].arguments.data);
                        cJSON_AddItemToObject(tc, "function", func);
                        cJSON_AddItemToArray(tcs, tc);
                    }
                    cJSON_AddItemToObject(json_msg, "tool_calls", tcs);
                }
            } else if (msg->role == ROLE_TOOL) {
                cJSON_AddStringToObject(json_msg, "role", "tool");
                cJSON_AddStringToObject(json_msg, "content", msg->content.data);
                if (msg->tool_call_id.len > 0) cJSON_AddStringToObject(json_msg, "tool_call_id", msg->tool_call_id.data);
                if (msg->name.len > 0) cJSON_AddStringToObject(json_msg, "name", msg->name.data);
            }
            cJSON_AddItemToArray(messages, json_msg);
        }
        session_snapshot_free(&snapshot);
    }
    return messages;
}

static void add_tools_to_json(cJSON* root, ToolRegistry* tools) {
    if (!tools || tools->count == 0) return;
    cJSON *tools_json = cJSON_CreateArray();
    for (size_t i = 0; i < tools->count; i++) {
        cJSON *tool_item = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_item, "type", "function");
        cJSON *func = cJSON_CreateObject();
        cJSON_AddStringToObject(func, "name", tools->tools[i].def.name.data);
        cJSON_AddStringToObject(func, "description", tools->tools[i].def.description.data);
        cJSON *params = cJSON_Parse(tools->tools[i].def.parameters.data);
        if (params) cJSON_AddItemToObject(func, "parameters", params);
        else cJSON_AddItemToObject(func, "parameters", cJSON_CreateObject());
        cJSON_AddItemToObject(tool_item, "function", func);
        cJSON_AddItemToArray(tools_json, tool_item);
    }
    cJSON_AddItemToObject(root, "tools", tools_json);
    cJSON_AddStringToObject(root, "tool_choice", "auto");
}

static char* build_llm_request_json(const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, bool stream) {
    cJSON *root = cJSON_CreateObject();
    ProviderConfig* pc = get_provider_config(config);
    const char* model = (pc && pc->model) ? pc->model : "gpt-4-turbo-preview";
    cJSON_AddStringToObject(root, "model", model);
    bool in_tool_chain = false;
    if (session) {
        SessionSnapshot snapshot = session_snapshot_for_context(session, 0);
        for (size_t i = 0; i < snapshot.count; i++) {
            Message* msg = snapshot.messages[i];
            if (msg->role == ROLE_TOOL) { in_tool_chain = true; break; }
        }
        session_snapshot_free(&snapshot);
    }
    if (pc) {
        cJSON_AddNumberToObject(root, "temperature", pc->temperature);
        if (in_tool_chain && pc->reasoning_effort && strlen(pc->reasoning_effort) > 0)
            cJSON_AddStringToObject(root, "reasoning_effort", pc->reasoning_effort);
    }
    cJSON_AddItemToObject(root, "messages", build_messages_json(system_prompt, session, config));
    add_tools_to_json(root, tools);
    if (stream) cJSON_AddBoolToObject(root, "stream", true);
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

static char* build_api_url(Config* config) {
    ProviderConfig* pc = get_provider_config(config);
    const char* api_base = (pc && pc->api_base && strlen(pc->api_base) > 0)
                           ? pc->api_base : "https://api.openai.com/v1";
    size_t url_len = strlen(api_base) + strlen("chat/completions") + 2;
    char* url = malloc(url_len);
    if (!url) return NULL;
    snprintf(url, url_len, "%s%schat/completions", api_base, api_base[strlen(api_base)-1] == '/' ? "" : "/");
    return url;
}

static ParsedLLMResponse parse_llm_response_json(const char* json_data, int http_status, const char* last_error) {
    ParsedLLMResponse result = {0};
    result.content = string_new("");
    result.finish_reason = FINISH_REASON_NONE;

    if (!json_data || strlen(json_data) == 0) {
        char errbuf[320];
        snprintf(errbuf, sizeof(errbuf), "Empty LLM response (status=%d, %s)", http_status, last_error[0] ? last_error : "no payload");
        result.error = error_new(ERR_NETWORK, errbuf);
        return result;
    }

    cJSON *json_response = cJSON_Parse(json_data);
    if (!json_response) { result.error = error_new(ERR_JSON, "Failed to parse LLM response"); return result; }

    cJSON *error_obj = cJSON_GetObjectItem(json_response, "error");
    if (error_obj) {
        cJSON *msg_item = cJSON_GetObjectItem(error_obj, "message");
        result.error = error_new(ERR_NETWORK, msg_item ? msg_item->valuestring : "Unknown API error");
        cJSON_Delete(json_response);
        return result;
    }

    cJSON *choices = cJSON_GetObjectItem(json_response, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "No choices in response (status=%d, object=%s, message=%s)",
                 http_status, json_get_string(json_response, "object", "n/a"), json_get_string(json_response, "message", "n/a"));
        result.error = error_new(ERR_JSON, errbuf);
        cJSON_Delete(json_response);
        return result;
    }

    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(choice, "message");

    cJSON *finish_reason_item = cJSON_GetObjectItem(choice, "finish_reason");
    if (finish_reason_item && cJSON_IsString(finish_reason_item))
        result.finish_reason = parse_finish_reason(finish_reason_item->valuestring);

    cJSON *usage = cJSON_GetObjectItem(json_response, "usage");
    if (usage) {
        cJSON *total_tokens = cJSON_GetObjectItem(usage, "total_tokens");
        if (total_tokens && cJSON_IsNumber(total_tokens)) result.usage_tokens = total_tokens->valueint;
    }

    if (result.finish_reason == FINISH_REASON_ERROR) {
        result.error = error_new(ERR_CONTENT_FILTER, "Content filtered by provider");
        cJSON_Delete(json_response);
        return result;
    }

    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (cJSON_IsString(content) && content->valuestring) {
        const char* trimmed = content->valuestring;
        while (*trimmed == '\n' || *trimmed == '\r') trimmed++;
        string_free(&result.content);
        result.content = string_new(trimmed);
    }

    cJSON *tcs = cJSON_GetObjectItem(message, "tool_calls");
    if (cJSON_IsArray(tcs)) {
        result.tool_calls_count = cJSON_GetArraySize(tcs);
        if (result.tool_calls_count > 0) {
            result.tool_calls = malloc(result.tool_calls_count * sizeof(ToolCall));
            int idx = 0;
            cJSON *tc;
            cJSON_ArrayForEach(tc, tcs) {
                cJSON *func = cJSON_GetObjectItem(tc, "function");
                cJSON *id_item = cJSON_GetObjectItem(tc, "id");
                cJSON *name_item = func ? cJSON_GetObjectItem(func, "name") : NULL;
                cJSON *args_item = func ? cJSON_GetObjectItem(func, "arguments") : NULL;
                if (!id_item || !cJSON_IsString(id_item) || !name_item || !cJSON_IsString(name_item) || !args_item || !cJSON_IsString(args_item)) {
                    result.tool_calls[idx].id = string_new("");
                    result.tool_calls[idx].name = string_new("");
                    result.tool_calls[idx].arguments = string_new("");
                } else {
                    result.tool_calls[idx].id = string_new(id_item->valuestring ? id_item->valuestring : "");
                    result.tool_calls[idx].name = string_new(name_item->valuestring ? name_item->valuestring : "");
                    result.tool_calls[idx].arguments = string_new(args_item->valuestring ? args_item->valuestring : "");
                }
                idx++;
            }
        }
    }
    cJSON_Delete(json_response);
    return result;
}

static void free_parsed_response(ParsedLLMResponse* r) {
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
            if (!new_mem) { snprintf(ms->last_error, sizeof(ms->last_error), "OOM in HTTP buffer"); ms->done = true; c->is_closing = 1; return; }
            ms->memory = new_mem; ms->capacity = new_cap;
        }
        memcpy(ms->memory + ms->size, hm->body.buf, hm->body.len);
        ms->size = new_size;
        ms->memory[ms->size] = '\0';
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

static void async_ev_handler(struct mg_connection *c, int ev, void *ev_data);

static void async_manager_add_active(LLMAsyncManager* manager, AsyncRequestContext* ctx) {
    if (!manager || !ctx) return;
    ctx->next_active = manager->active_head;
    manager->active_head = ctx;
}

static void async_manager_remove_active(LLMAsyncManager* manager, AsyncRequestContext* ctx) {
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

static void async_context_callback_once(AsyncRequestContext* ctx, Error err, const char* response, ToolCall* tool_calls, size_t tool_calls_count) {
    if (!ctx || ctx->callback_sent) return;
    ctx->callback_sent = true;
    if (ctx->callback) {
        ctx->callback(err, response, tool_calls, tool_calls_count, ctx->user_data);
    }
}

static void async_manager_check_timeouts(LLMAsyncManager* manager) {
    if (!manager) return;
    uint64_t now = mg_millis();
    struct mg_connection* c = manager->mgr.conns;
    while (c) {
        struct mg_connection* next = c->next;
        AsyncRequestContext* ctx = (AsyncRequestContext*)c->fn_data;
        if (ctx && ctx->timeout_ms > 0 && now - ctx->start_ms >= ctx->timeout_ms) {
            snprintf(ctx->chunk.last_error, sizeof(ctx->chunk.last_error), "LLM request timed out");
            ctx->chunk.done = true;
            c->is_closing = 1;
        }
        c = next;
    }
}

static LLMAsyncManager* llm_async_manager_new(void) {
    LLMAsyncManager* manager = calloc(1, sizeof(LLMAsyncManager));
    if (!manager) return NULL;
    mg_mgr_init(&manager->mgr);
    manager->running = false;
    pthread_mutex_init(&manager->queue_mutex, NULL);
    pthread_mutex_init(&manager->dns_mutex, NULL);
    pthread_cond_init(&manager->queue_cond, NULL);
    manager->dns_configured = false;
    return manager;
}

static void llm_async_manager_free(LLMAsyncManager* manager) {
    if (!manager) return;
    if (manager->running) llm_async_manager_stop(manager);
    mg_mgr_free(&manager->mgr);
    pthread_mutex_destroy(&manager->queue_mutex);
    pthread_mutex_destroy(&manager->dns_mutex);
    pthread_cond_destroy(&manager->queue_cond);
    free(manager);
}

static void async_worker_process_item(AsyncQueueItem* item, LLMAsyncManager* manager) {
    if (!item || !item->ctx) return;
    pthread_mutex_lock(&manager->dns_mutex);
    if (!manager->dns_configured && item->config) {
        configure_mgr_dns(&manager->mgr, item->config);
        manager->dns_configured = true;
    }
    pthread_mutex_unlock(&manager->dns_mutex);
    struct mg_connection *c = mg_http_connect(&manager->mgr, item->url, async_ev_handler, item->ctx);
    if (!c) {
        if (item->ctx->callback) item->ctx->callback(error_new(ERR_NETWORK, "Failed to connect to LLM"), NULL, NULL, 0, item->ctx->user_data);
        free(item->ctx->chunk.memory);
        free(item->ctx);
    } else {
        async_manager_add_active(manager, item->ctx);
        struct mg_str host = mg_url_host(item->url);
        struct mg_tls_opts opts = {
            .ca = mg_str(""),
            .cert = {0},
            .key = {0},
            .name = host,
            .skip_verification = item->skip_tls_verify ? 1 : 0,
        };
        if (mg_url_is_ssl(item->url)) mg_tls_init(c, &opts);

    mg_printf(c, "POST %s HTTP/1.0\r\nHost: %.*s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nAuthorization: Bearer %s\r\n\r\n%s",
        mg_url_uri(item->url), (int)host.len, host.buf, (int)strlen(item->json_str), item->api_key, item->json_str);
    }
    free(item->url); free(item->json_str); free(item->api_key);
    free(item);
}

static AsyncQueueItem* async_worker_dequeue_all(LLMAsyncManager* manager) {
    pthread_mutex_lock(&manager->queue_mutex);
    AsyncQueueItem* items = manager->queue_head;
    manager->queue_head = NULL; manager->queue_tail = NULL;
    pthread_mutex_unlock(&manager->queue_mutex);
    return items;
}

static void* llm_async_worker(void* arg) {
    LLMAsyncManager* manager = (LLMAsyncManager*)arg;
    if (!manager) return NULL;
    while (manager->running) {
        AsyncQueueItem* items = async_worker_dequeue_all(manager);
        while (items) { AsyncQueueItem* next = items->next; async_worker_process_item(items, manager); items = next; }
        mg_mgr_poll(&manager->mgr, 10);
        async_manager_check_timeouts(manager);
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
        async_context_callback_once(active, error_new(ERR_NETWORK, "Shutting down"), NULL, NULL, 0);
        free(active->chunk.memory);
        free(active);
        active = next;
    }
    return NULL;
}

static void llm_async_manager_start(LLMAsyncManager* manager) {
    if (!manager || manager->running) return;
    manager->running = true;
    pthread_create(&manager->thread, NULL, llm_async_worker, manager);
}

static void llm_async_manager_stop(LLMAsyncManager* manager) {
    if (!manager || !manager->running) return;
    manager->running = false;
    pthread_mutex_lock(&manager->queue_mutex); pthread_cond_signal(&manager->queue_cond); pthread_mutex_unlock(&manager->queue_mutex);
    pthread_join(manager->thread, NULL);
}

// =============================================================================
// Async Event Handler
// =============================================================================

static void async_ev_handler(struct mg_connection *c, int ev, void *ev_data) {
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
            if (!new_mem) { snprintf(ms->last_error, sizeof(ms->last_error), "OOM in HTTP buffer"); ms->done = true; c->is_closing = 1; return; }
            ms->memory = new_mem; ms->capacity = new_cap;
        }
        memcpy(ms->memory + ms->size, hm->body.buf, hm->body.len);
        ms->size = new_size; ms->memory[ms->size] = '\0';
        c->is_closing = 1; ms->done = true;
    } else if (ev == MG_EV_CLOSE) {
        if (ms->size == 0 && !ms->done) ms->done = true;
        ParsedLLMResponse parsed = parse_llm_response_json(ms->size > 0 ? ms->memory : NULL, ms->http_status, ms->last_error[0] ? ms->last_error : "no payload");
        if (ctx->manager) {
            async_manager_remove_active(ctx->manager, ctx);
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
            async_context_callback_once(ctx, parsed.error, response_copy, tool_calls_copy, tool_calls_count_copy);
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
// Streaming Event Handler
// =============================================================================

static void stream_ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    StreamContext *ctx = (StreamContext *)c->fn_data;

    if (ev == MG_EV_READ) {
        char buf[2048];
        long n = mg_io_recv(c, buf, sizeof(buf));
        if (n > 0) {
            size_t old_len = ctx->recv_buf.len;
            ctx->recv_buf.data = realloc(ctx->recv_buf.data, old_len + n + 1);
            if (!ctx->recv_buf.data) return;
            memcpy(ctx->recv_buf.data + old_len, buf, n);
            ctx->recv_buf.len = old_len + n; ctx->recv_buf.data[ctx->recv_buf.len] = '\0';
            char* line_start = ctx->recv_buf.data;
            char* newline;
            while ((newline = strchr(line_start, '\n')) != NULL) {
                *newline = '\0';
                if (strncmp(line_start, "data: ", 6) == 0) {
                    char* data = line_start + 6;
                    size_t data_len = strlen(data);
                    if (data_len > 0 && data[data_len-1] == '\r') { data[data_len-1] = '\0'; data_len--; }
                    if (strcmp(data, "[DONE]") == 0) { ctx->done = true; if (ctx->on_chunk) ctx->on_chunk("", 0, true, ctx->user_data); break; }
                    cJSON *json = cJSON_Parse(data);
                    if (json) {
                        cJSON *choices = cJSON_GetObjectItem(json, "choices");
                        if (choices && cJSON_GetArraySize(choices) > 0) {
                            cJSON *delta = cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "delta");
                            if (delta) {
                                cJSON *content_item = cJSON_GetObjectItem(delta, "content");
                                if (content_item && cJSON_IsString(content_item) && content_item->valuestring) {
                                    string_append(&ctx->accumulated_content, content_item->valuestring);
                                    if (ctx->on_chunk) ctx->on_chunk(content_item->valuestring, strlen(content_item->valuestring), false, ctx->user_data);
                                }
                                cJSON *tool_calls = cJSON_GetObjectItem(delta, "tool_calls");
                                if (tool_calls && cJSON_GetArraySize(tool_calls) > 0) {
                                    int tc_count = cJSON_GetArraySize(tool_calls);
                                    for (int ti = 0; ti < tc_count; ti++) {
                                        cJSON* tc_delta = cJSON_GetArrayItem(tool_calls, ti);
                                        if (!tc_delta) continue;
                                        cJSON* index_item = cJSON_GetObjectItem(tc_delta, "index");
                                        int tc_index = index_item && cJSON_IsNumber(index_item) ? index_item->valueint : (int)ctx->tool_calls_accum_count;
                                        if (tc_index >= (int)ctx->tool_calls_accum_capacity) {
                                            size_t new_cap = tc_index + 4;
                                            ToolCall* new_arr = realloc(ctx->tool_calls_accum, new_cap * sizeof(ToolCall));
                                            if (!new_arr) continue;
                                            for (size_t k = ctx->tool_calls_accum_capacity; k < new_cap; k++) {
                                                memset(&new_arr[k], 0, sizeof(ToolCall));
                                                new_arr[k].id = string_new(""); new_arr[k].name = string_new(""); new_arr[k].arguments = string_new("");
                                            }
                                            ctx->tool_calls_accum = new_arr; ctx->tool_calls_accum_capacity = new_cap;
                                        }
                                        if (tc_index >= (int)ctx->tool_calls_accum_count) ctx->tool_calls_accum_count = tc_index + 1;
                                        ToolCall* tc = &ctx->tool_calls_accum[tc_index];
                                        cJSON* id_item = cJSON_GetObjectItem(tc_delta, "id");
                                        if (id_item && cJSON_IsString(id_item) && id_item->valuestring) { string_free(&tc->id); tc->id = string_new(id_item->valuestring); }
                                        cJSON* func_delta = cJSON_GetObjectItem(tc_delta, "function");
                                        if (func_delta) {
                                            cJSON* name_item = cJSON_GetObjectItem(func_delta, "name");
                                            if (name_item && cJSON_IsString(name_item)) { string_free(&tc->name); tc->name = string_new(name_item->valuestring); }
                                            cJSON* args_item = cJSON_GetObjectItem(func_delta, "arguments");
                                            if (args_item && cJSON_IsString(args_item)) string_append(&tc->arguments, args_item->valuestring);
                                        }
                                    }
                                }
                            }
                            cJSON *finish_item = cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "finish_reason");
                            if (finish_item && cJSON_IsString(finish_item)) ctx->llm_response->finish_reason = parse_finish_reason(finish_item->valuestring);
                        }
                        cJSON *usage = cJSON_GetObjectItem(json, "usage");
                        if (usage) { cJSON *total = cJSON_GetObjectItem(usage, "total_tokens"); if (total && cJSON_IsNumber(total)) ctx->llm_response->usage_tokens = total->valueint; }
                        cJSON_Delete(json);
                    }
                }
                line_start = newline + 1;
            }
            if (line_start > ctx->recv_buf.data) {
                size_t remaining = ctx->recv_buf.len - (line_start - ctx->recv_buf.data);
                if (remaining > 0) memmove(ctx->recv_buf.data, line_start, remaining);
                ctx->recv_buf.len = remaining; ctx->recv_buf.data[ctx->recv_buf.len] = '\0';
            }
        }
    } else if (ev == MG_EV_CLOSE) {
        ctx->done = true;
    } else if (ev == MG_EV_ERROR) {
        snprintf(ctx->last_error, sizeof(ctx->last_error), "%s", ev_data ? (const char*)ev_data : "unknown");
        ctx->done = true;
    }
}

// =============================================================================
// Stream Context Cleanup Helper
// =============================================================================

static void stream_context_cleanup(StreamContext* ctx) {
    if (!ctx) return;
    string_free(&ctx->accumulated_content);
    string_free(&ctx->recv_buf);
    if (ctx->tool_calls_accum) {
        for (size_t i = 0; i < ctx->tool_calls_accum_count; i++) {
            string_free(&ctx->tool_calls_accum[i].id);
            string_free(&ctx->tool_calls_accum[i].name);
            string_free(&ctx->tool_calls_accum[i].arguments);
        }
        free(ctx->tool_calls_accum);
        ctx->tool_calls_accum = NULL;
        ctx->tool_calls_accum_count = 0;
        ctx->tool_calls_accum_capacity = 0;
    }
}

// =============================================================================
// Provider Interface Implementation
// =============================================================================

static Error openai_provider_init(LLMProvider* provider, Config* config) {
    (void)config;
    if (!provider) return error_new(ERR_INVALID_PARAM, "provider is NULL");
    LLMAsyncManager* manager = llm_async_manager_new();
    if (!manager) return error_new(ERR_NETWORK, "Failed to create async manager");
    llm_async_manager_start(manager);
    provider->state = manager;
    provider->initialized = true;
    return error_new(ERR_NONE, "");
}

static void openai_provider_shutdown(LLMProvider* provider) {
    if (!provider) return;
    LLMAsyncManager* manager = (LLMAsyncManager*)provider->state;
    if (manager) { llm_async_manager_stop(manager); llm_async_manager_free(manager); provider->state = NULL; }
    provider->initialized = false;
}

static Error openai_provider_call(LLMProvider* provider, const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, String* response, ToolCall** tool_calls, size_t* tool_calls_count) {
    LLMResponse llm_response;
    Error err = openai_provider_call_extended(provider, system_prompt, session, tools, config, &llm_response);
    if (err.code != ERR_NONE) return err;
    *response = llm_response.content;
    *tool_calls = llm_response.tool_calls;
    *tool_calls_count = llm_response.tool_calls_count;
    return error_new(ERR_NONE, "");
}

static Error openai_provider_call_extended(LLMProvider* provider, const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, LLMResponse* llm_response) {
    (void)provider;
    if (!llm_response) return error_new(ERR_INVALID_PARAM, "llm_response is NULL");

    llm_response->content = string_new("");
    llm_response->tool_calls = NULL; llm_response->tool_calls_count = 0;
    llm_response->finish_reason = FINISH_REASON_NONE; llm_response->usage_tokens = 0;

    struct mg_mgr mgr;
    MemoryStruct chunk = {0};
    chunk.capacity = 4096;
    chunk.memory = malloc(chunk.capacity);
    if (!chunk.memory) return error_new(ERR_MEMORY, "OOM allocating response buffer");
    chunk.memory[0] = '\0';

    mg_mgr_init(&mgr);
    configure_mgr_dns(&mgr, config);

    const char* api_key = get_api_key(config);
    if (strlen(api_key) == 0) { mg_mgr_free(&mgr); free(chunk.memory); return error_new(ERR_INVALID_PARAM, "API Key not set"); }

    char *json_str = build_llm_request_json(system_prompt, session, tools, config, false);
    char* url = build_api_url(config);
    if (!url) { free(json_str); mg_mgr_free(&mgr); free(chunk.memory); return error_new(ERR_NETWORK, "OOM building URL"); }

    struct mg_connection *c = mg_http_connect(&mgr, url, sync_ev_handler, &chunk);
    if (!c) { free(url); free(json_str); mg_mgr_free(&mgr); free(chunk.memory); return error_new(ERR_NETWORK, "Failed to connect"); }

    struct mg_str host = mg_url_host(url);
    struct mg_tls_opts opts = {
        .ca = mg_str(""),
        .cert = {0},
        .key = {0},
        .name = host,
        .skip_verification = should_skip_tls_verification() ? 1 : 0,
    };
    if (mg_url_is_ssl(url)) mg_tls_init(c, &opts);

    mg_printf(c, "POST %s HTTP/1.0\r\nHost: %.*s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nAuthorization: Bearer %s\r\n\r\n%s",
        mg_url_uri(url), (int)host.len, host.buf, (int)strlen(json_str), api_key, json_str);

    { uint64_t start_ms = mg_millis(); while (!chunk.done && (mg_millis() - start_ms) < 120000) mg_mgr_poll(&mgr, 1000); }

    free(url); free(json_str); mg_mgr_free(&mgr);
    ParsedLLMResponse parsed = parse_llm_response_json(chunk.memory, chunk.http_status, chunk.last_error);
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

static void openai_provider_call_async(LLMProvider* provider, const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, LLMAsyncCallback callback, void* user_data) {
    if (!provider || !provider->state) { if (callback) callback(error_new(ERR_INVALID_PARAM, "Provider not initialized"), NULL, NULL, 0, user_data); return; }
    LLMAsyncManager* manager = (LLMAsyncManager*)provider->state;
    const char* api_key = get_api_key(config);
    if (strlen(api_key) == 0) { if (callback) callback(error_new(ERR_INVALID_PARAM, "API Key not set"), NULL, NULL, 0, user_data); return; }

    char *json_str = build_llm_request_json(system_prompt, session, tools, config, false);
    char* url = build_api_url(config);
    if (!url || !json_str) { free(json_str); free(url); if (callback) callback(error_new(ERR_NETWORK, "OOM building request"), NULL, NULL, 0, user_data); return; }

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

static Error openai_provider_call_streaming(LLMProvider* provider, const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, LLMResponse* llm_response, LLMStreamOptions* options) {
    (void)provider;
    if (!llm_response) return error_new(ERR_INVALID_PARAM, "llm_response is NULL");
    if (!options || !options->on_chunk) return error_new(ERR_INVALID_PARAM, "streaming callback required");

    llm_response->content = string_new(""); llm_response->tool_calls = NULL; llm_response->tool_calls_count = 0;
    llm_response->finish_reason = FINISH_REASON_NONE; llm_response->usage_tokens = 0;

    StreamContext ctx = {0};
    ctx.llm_response = llm_response; ctx.on_chunk = options->on_chunk; ctx.user_data = options->user_data;
    ctx.done = false; ctx.accumulated_content = string_new(""); ctx.recv_buf = string_new("");

    struct mg_mgr mgr; mg_mgr_init(&mgr); configure_mgr_dns(&mgr, config);
    const char* api_key = get_api_key(config);
    if (strlen(api_key) == 0) { mg_mgr_free(&mgr); stream_context_cleanup(&ctx); return error_new(ERR_INVALID_PARAM, "API Key not set"); }

    char *json_str = build_llm_request_json(system_prompt, session, tools, config, true);
    char* url = build_api_url(config);
    if (!url) { free(json_str); mg_mgr_free(&mgr); stream_context_cleanup(&ctx); return error_new(ERR_NETWORK, "OOM building URL"); }

    struct mg_connection *c = mg_http_connect(&mgr, url, stream_ev_handler, &ctx);
    if (!c) { free(url); free(json_str); mg_mgr_free(&mgr); stream_context_cleanup(&ctx); return error_new(ERR_NETWORK, "Failed to connect"); }

    struct mg_str host = mg_url_host(url);
    struct mg_tls_opts opts = {
        .ca = mg_str(""),
        .cert = {0},
        .key = {0},
        .name = host,
        .skip_verification = should_skip_tls_verification() ? 1 : 0,
    };
    if (mg_url_is_ssl(url)) mg_tls_init(c, &opts);
    mg_printf(c, "POST %s HTTP/1.1\r\nHost: %.*s\r\nContent-Type: application/json\r\nAccept: text/event-stream\r\nAuthorization: Bearer %s\r\nConnection: keep-alive\r\n\r\n%s",
        mg_url_uri(url), (int)host.len, host.buf, api_key, json_str);
    free(url); free(json_str);

    { uint64_t start_ms = mg_millis(); while (!ctx.done && (mg_millis() - start_ms) < 120000) mg_mgr_poll(&mgr, 100); }
    mg_mgr_free(&mgr);

    if (ctx.last_error[0]) {
        stream_context_cleanup(&ctx);
        return error_new(ERR_NETWORK, ctx.last_error);
    }
    char* trimmed = ctx.accumulated_content.data; while (*trimmed == '\n' || *trimmed == '\r') trimmed++;
    llm_response->content = string_new(trimmed);
    string_free(&ctx.accumulated_content); string_free(&ctx.recv_buf);

    if (ctx.tool_calls_accum_count > 0 && ctx.tool_calls_accum) {
        llm_response->tool_calls_count = ctx.tool_calls_accum_count;
        llm_response->tool_calls = calloc(ctx.tool_calls_accum_count, sizeof(ToolCall));
        if (llm_response->tool_calls) for (size_t i = 0; i < ctx.tool_calls_accum_count; i++) {
            llm_response->tool_calls[i].id = string_copy(&ctx.tool_calls_accum[i].id);
            llm_response->tool_calls[i].name = string_copy(&ctx.tool_calls_accum[i].name);
            llm_response->tool_calls[i].arguments = string_copy(&ctx.tool_calls_accum[i].arguments);
        } else llm_response->tool_calls_count = 0;
        for (size_t i = 0; i < ctx.tool_calls_accum_count; i++) { string_free(&ctx.tool_calls_accum[i].id); string_free(&ctx.tool_calls_accum[i].name); string_free(&ctx.tool_calls_accum[i].arguments); }
        free(ctx.tool_calls_accum);
    }
    if (llm_response->finish_reason == FINISH_REASON_CONTENT_FILTER) return error_new(ERR_CONTENT_FILTER, "Content filtered");
    return error_new(ERR_NONE, "");
}

const LLMProviderInterface OPENAI_PROVIDER_INTERFACE = {
    .name = "OpenAI Compatible",
    .init = openai_provider_init,
    .shutdown = openai_provider_shutdown,
    .call = openai_provider_call,
    .call_extended = openai_provider_call_extended,
    .call_async = openai_provider_call_async,
    .call_streaming = openai_provider_call_streaming,
};
