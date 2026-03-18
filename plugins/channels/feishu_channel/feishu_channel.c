/*
 * Feishu Channel Plugin
 *
 * A plugin that demonstrates the Primagen channel plugin API.
 * This channel sends and receives messages via Feishu (Lark) bot.
 *
 * Build: make
 * Install: cp feishu_channel.so ../../.primagen/plugins/channels/
 */

#include "../../../src/include/channel.h"
#include "../../../src/include/common.h"
#include "../../../src/include/logger.h"
#include "../../../src/include/config.h"
#include "../../../src/plugin/plugin_manager.h"
#include "../../../src/bus/message_bus.h"
#include "../../../src/include/message.h"
#include "../../../src/vendor/cJSON/cJSON.h"
#include "../../../src/vendor/mongoose/mongoose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

// =============================================================================
// Forward Declarations
// =============================================================================

typedef struct FeishuWS FeishuWS;

// Callback for received text messages
typedef void (*FeishuWSMessageCallback)(const char* chat_id, const char* content, const char* sender_id, void* user_data);

FeishuWS* feishu_ws_create(void);
void feishu_ws_destroy(FeishuWS* ws);
bool feishu_ws_connect(FeishuWS* ws, const char* url);
void feishu_ws_run(FeishuWS* ws, FeishuWSMessageCallback callback, void* user_data);
void feishu_ws_stop(FeishuWS* ws);

// =============================================================================
// Channel Data
// =============================================================================

typedef struct {
    MessageBus* bus;
    bool running;
    char* access_token;
    pthread_t thread_id;
    FeishuWS* ws;
    // Plugin configuration
    char* app_id;
    char* app_secret;
    bool use_card;
} FeishuChannelData;

struct MemoryStruct {
    char *memory;
    size_t size;
    bool done;
};

// =============================================================================
// Internal Helper Functions
// =============================================================================

static void fn(struct mg_connection *c, int ev, void *ev_data) {
    struct MemoryStruct *ms = (struct MemoryStruct *) c->fn_data;
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        size_t new_size = ms->size + hm->body.len;
        ms->memory = realloc(ms->memory, new_size + 1);
        memcpy(ms->memory + ms->size, hm->body.buf, hm->body.len);
        ms->size = new_size;
        ms->memory[ms->size] = '\0';
        c->is_closing = 1;
        ms->done = true;
    } else if (ev == MG_EV_ERROR) {
        ms->done = true;
    }
}

static void refresh_token(FeishuChannelData* data) {
    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';

    mg_mgr_init(&mgr);

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "app_id", data->app_id);
    cJSON_AddStringToObject(payload, "app_secret", data->app_secret);
    char* json_str = cJSON_PrintUnformatted(payload);

    const char* url = "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal";

    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        log_error("[Feishu] Failed to connect for token refresh");
        free(json_str);
        cJSON_Delete(payload);
        mg_mgr_free(&mgr);
        free(chunk.memory);
        return;
    }

    struct mg_str host = mg_url_host(url);

    struct mg_tls_opts opts = {0};
    opts.ca = mg_str("");
    opts.name = host;
    opts.skip_verification = true;
    if (mg_url_is_ssl(url)) {
        mg_tls_init(c, &opts);
    }

    mg_printf(c,
        "POST %s HTTP/1.0\r\n"
        "Host: %.*s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        mg_url_uri(url),
        (int)host.len, host.buf,
        (int)strlen(json_str),
        json_str
    );

    while (!chunk.done) mg_mgr_poll(&mgr, 1000);

    if (chunk.size > 0) {
        cJSON* resp = cJSON_Parse(chunk.memory);
        if (resp) {
            cJSON* code = cJSON_GetObjectItem(resp, "code");
            if (code && code->valueint != 0) {
                cJSON* msg = cJSON_GetObjectItem(resp, "msg");
                log_error("[Feishu] Token refresh failed: %d - %s",
                        code->valueint, msg ? msg->valuestring : "Unknown error");
            } else {
                cJSON* token = cJSON_GetObjectItem(resp, "tenant_access_token");
                if (cJSON_IsString(token)) {
                    if (data->access_token) free(data->access_token);
                    data->access_token = strdup(token->valuestring);
                }
            }
            cJSON_Delete(resp);
        } else {
            log_error("[Feishu] Failed to parse token response. Raw body: '%s'", chunk.memory);
        }
    } else {
        log_error("[Feishu] Empty response for token refresh");
    }

    free(chunk.memory);
    free(json_str);
    cJSON_Delete(payload);
    mg_mgr_free(&mgr);
}

static char* get_ws_url(FeishuChannelData* data) {
    if (!data->access_token) refresh_token(data);
    char* ws_url = NULL;

    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';

    mg_mgr_init(&mgr);

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "AppID", data->app_id);
    cJSON_AddStringToObject(payload, "AppSecret", data->app_secret);
    char* json_str = cJSON_PrintUnformatted(payload);

    const char* url = "https://open.feishu.cn/callback/ws/endpoint";

    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        log_error("[Feishu] Failed to connect for WS URL");
        free(json_str);
        cJSON_Delete(payload);
        mg_mgr_free(&mgr);
        free(chunk.memory);
        return NULL;
    }

    struct mg_str host = mg_url_host(url);

    struct mg_tls_opts opts = {0};
    opts.ca = mg_str("");
    opts.name = host;
    opts.skip_verification = true;
    if (mg_url_is_ssl(url)) {
        mg_tls_init(c, &opts);
    }

    mg_printf(c,
        "POST %s HTTP/1.0\r\n"
        "Host: %.*s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "locale: zh\r\n"
        "\r\n"
        "%s",
        mg_url_uri(url),
        (int)host.len, host.buf,
        (int)strlen(json_str),
        json_str
    );

    while (!chunk.done) mg_mgr_poll(&mgr, 1000);

    if (chunk.size > 0) {
        cJSON* resp = cJSON_Parse(chunk.memory);
        if (resp) {
            cJSON* code = cJSON_GetObjectItem(resp, "code");
            if (code && code->valueint == 0) {
                cJSON* dataObj = cJSON_GetObjectItem(resp, "data");
                if (dataObj) {
                    cJSON* urlItem = cJSON_GetObjectItem(dataObj, "URL");
                    if (cJSON_IsString(urlItem)) {
                        ws_url = strdup(urlItem->valuestring);
                    }
                }
            } else {
                cJSON* msg = cJSON_GetObjectItem(resp, "msg");
                log_error("[Feishu] Get WS URL failed: %d - %s",
                        code ? code->valueint : -1, msg ? msg->valuestring : "Unknown");
            }
            cJSON_Delete(resp);
        } else {
            log_error("[Feishu] Failed to parse WS URL response. Raw body: '%s'", chunk.memory);
        }
    }

    free(chunk.memory);
    free(json_str);
    cJSON_Delete(payload);
    mg_mgr_free(&mgr);

    return ws_url;
}

static char* create_streaming_card(FeishuChannelData* data, const char* content) {
    if (!data->access_token) refresh_token(data);
    char* card_id = NULL;

    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';

    mg_mgr_init(&mgr);

    // Construct Card JSON
    cJSON* cardData = cJSON_CreateObject();
    cJSON_AddStringToObject(cardData, "schema", "2.0");

    cJSON* header = cJSON_CreateObject();
    cJSON_AddStringToObject(header, "template", "blue");
    cJSON* title = cJSON_CreateObject();
    cJSON_AddStringToObject(title, "tag", "plain_text");
    cJSON_AddStringToObject(title, "content", "Primagen");
    cJSON_AddItemToObject(header, "title", title);
    cJSON_AddItemToObject(cardData, "header", header);

    cJSON* config = cJSON_CreateObject();
    cJSON_AddBoolToObject(config, "streaming_mode", true);
    cJSON_AddItemToObject(cardData, "config", config);

    cJSON* body = cJSON_CreateObject();
    cJSON* elements = cJSON_CreateArray();
    cJSON* element = cJSON_CreateObject();
    cJSON_AddStringToObject(element, "tag", "markdown");
    cJSON_AddStringToObject(element, "element_id", "markdown_1");
    cJSON_AddStringToObject(element, "content", content);
    cJSON_AddItemToArray(elements, element);
    cJSON_AddItemToObject(body, "elements", elements);
    cJSON_AddItemToObject(cardData, "body", body);

    char* card_data_str = cJSON_PrintUnformatted(cardData);

    cJSON* reqBody = cJSON_CreateObject();
    cJSON_AddStringToObject(reqBody, "type", "card_json");
    cJSON_AddStringToObject(reqBody, "data", card_data_str);
    char* req_json_str = cJSON_PrintUnformatted(reqBody);

    const char* url = "https://open.feishu.cn/open-apis/cardkit/v1/cards";

    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        log_error("[Feishu] Failed to connect for Card creation");
        free(req_json_str);
        free(card_data_str);
        cJSON_Delete(reqBody);
        cJSON_Delete(cardData);
        mg_mgr_free(&mgr);
        free(chunk.memory);
        return NULL;
    }

    struct mg_str host = mg_url_host(url);

    struct mg_tls_opts opts = {0};
    opts.ca = mg_str("");
    opts.name = host;
    opts.skip_verification = true;
    if (mg_url_is_ssl(url)) {
        mg_tls_init(c, &opts);
    }

    mg_printf(c,
        "POST %s HTTP/1.0\r\n"
        "Host: %.*s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        mg_url_uri(url),
        (int)host.len, host.buf,
        data->access_token ? data->access_token : "",
        (int)strlen(req_json_str),
        req_json_str
    );

    while (!chunk.done) mg_mgr_poll(&mgr, 1000);

    if (chunk.size > 0) {
        cJSON* resp = cJSON_Parse(chunk.memory);
        if (resp) {
            cJSON* code = cJSON_GetObjectItem(resp, "code");
            if (code && code->valueint == 0) {
                cJSON* dataObj = cJSON_GetObjectItem(resp, "data");
                if (dataObj) {
                    cJSON* id = cJSON_GetObjectItem(dataObj, "card_id");
                    if (cJSON_IsString(id)) {
                        card_id = strdup(id->valuestring);
                    }
                }
            } else {
                cJSON* msg = cJSON_GetObjectItem(resp, "msg");
                log_error("[Feishu] Create Card failed: %d - %s",
                        code ? code->valueint : -1, msg ? msg->valuestring : "Unknown");
            }
            cJSON_Delete(resp);
        }
    }

    free(chunk.memory);
    free(card_data_str);
    free(req_json_str);
    cJSON_Delete(cardData);
    cJSON_Delete(reqBody);
    mg_mgr_free(&mgr);

    return card_id;
}

static void on_feishu_message(const char* chat_id, const char* content, const char* sender_id, void* user_data) {
    FeishuChannelData* data = (FeishuChannelData*)user_data;
    log_info("[Feishu] Received from %s: %s", sender_id, content);

    // Create InboundMessage
    InboundMessage* msg = inbound_message_new("feishu", chat_id, content);
    message_bus_send_inbound(data->bus, msg);
}

static void* feishu_receive_loop(void* arg) {
    FeishuChannelData* data = (FeishuChannelData*)arg;

    while (data->running) {
        char* url = get_ws_url(data);
        if (url) {
            log_info("[Feishu] Connecting to WebSocket...");
            data->ws = feishu_ws_create();
            if (feishu_ws_connect(data->ws, url)) {
                log_info("[Feishu] WebSocket connected.");
                feishu_ws_run(data->ws, on_feishu_message, data);
            } else {
                log_error("[Feishu] WebSocket connect failed.");
            }
            feishu_ws_destroy(data->ws);
            data->ws = NULL;
            free(url);
        } else {
            log_error("[Feishu] Failed to get WebSocket URL");
        }

        if (data->running) sleep(5);
    }
    return NULL;
}

// =============================================================================
// Channel Implementation
// =============================================================================

static bool feishu_init(Channel* self, Config* cfg, MessageBus* bus) {
    FeishuChannelData* data = malloc(sizeof(FeishuChannelData));
    if (!data) return false;

    data->bus = bus;
    data->running = false;
    data->access_token = NULL;
    data->ws = NULL;

    // Get plugin configuration
    PluginConfig* plugin_cfg = config_get_plugin_config(cfg, "feishu_channel");
    if (plugin_cfg && plugin_cfg->config) {
        cJSON* app_id = cJSON_GetObjectItem(plugin_cfg->config, "app_id");
        cJSON* app_secret = cJSON_GetObjectItem(plugin_cfg->config, "app_secret");
        cJSON* use_card = cJSON_GetObjectItem(plugin_cfg->config, "use_card");

        data->app_id = app_id && cJSON_IsString(app_id) ? strdup(app_id->valuestring) : strdup("");
        data->app_secret = app_secret && cJSON_IsString(app_secret) ? strdup(app_secret->valuestring) : strdup("");
        data->use_card = use_card && cJSON_IsBool(use_card) ? use_card->valueint : false;
    } else {
        data->app_id = strdup("");
        data->app_secret = strdup("");
        data->use_card = false;
    }

    self->user_data = data;
    log_info("[Feishu] Initialized with app_id: %s", data->app_id);
    return true;
}

static void feishu_start(Channel* self) {
    FeishuChannelData* data = (FeishuChannelData*)self->user_data;
    // Check if app_id is configured (plugin is enabled if app_id is set)
    if (!data->app_id || !data->app_id[0]) {
        log_info("[Feishu] Channel not started (no app_id configured)");
        return;
    }

    data->running = true;
    refresh_token(data);

    // Start receive thread
    pthread_create(&data->thread_id, NULL, feishu_receive_loop, data);
}

static void feishu_stop(Channel* self) {
    FeishuChannelData* data = (FeishuChannelData*)self->user_data;
    data->running = false;
    if (data->ws) feishu_ws_stop(data->ws);
}

static void feishu_send(Channel* self, OutboundMessage* msg) {
    FeishuChannelData* data = (FeishuChannelData*)self->user_data;
    // Check if channel is configured
    if (!data->app_id || !data->app_id[0]) return;

    if (strcmp(msg->channel.data, "feishu") != 0) return;

    if (!data->access_token) refresh_token(data);

    if (!data->access_token) {
        log_error("[Feishu] Not connected (no token)");
        return;
    }

    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';

    mg_mgr_init(&mgr);

    char url[512];
    const char* id_type = "open_id";
    if (strncmp(msg->chat_id.data, "oc_", 3) == 0) {
        id_type = "chat_id";
    } else if (strncmp(msg->chat_id.data, "ou_", 3) == 0) {
        id_type = "open_id";
    }

    snprintf(url, sizeof(url), "https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=%s", id_type);

    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "receive_id", msg->chat_id.data);

    bool sent_card = false;
    if (data->use_card) {
        char* card_id = create_streaming_card(data, msg->content.data);
        if (card_id) {
            cJSON_AddStringToObject(json, "msg_type", "interactive");
            cJSON* contentObj = cJSON_CreateObject();
            cJSON_AddStringToObject(contentObj, "type", "card");
            cJSON* cardObj = cJSON_CreateObject();
            cJSON_AddStringToObject(cardObj, "card_id", card_id);
            cJSON_AddItemToObject(contentObj, "data", cardObj);
            char* content_str = cJSON_PrintUnformatted(contentObj);
            cJSON_AddStringToObject(json, "content", content_str);
            free(content_str);
            cJSON_Delete(contentObj);
            free(card_id);
            sent_card = true;
        }
    }

    if (!sent_card) {
        // Use post message type with rich text format
        // Note: zh_cn needs title and content (2D array for paragraphs)
        cJSON_AddStringToObject(json, "msg_type", "post");
        cJSON* contentObj = cJSON_CreateObject();
        cJSON* zhCN = cJSON_CreateObject();

        // Add required title field
        cJSON_AddStringToObject(zhCN, "title", "");

        // Content is a 2D array - each inner array is a paragraph
        cJSON* content = cJSON_CreateArray();
        cJSON* paragraph = cJSON_CreateArray();  // Each paragraph is an array of elements
        cJSON* textItem = cJSON_CreateObject();
        cJSON_AddStringToObject(textItem, "tag", "text");
        cJSON_AddStringToObject(textItem, "text", msg->content.data);
        cJSON_AddItemToArray(paragraph, textItem);
        cJSON_AddItemToArray(content, paragraph);
        cJSON_AddItemToObject(zhCN, "content", content);

        cJSON_AddItemToObject(contentObj, "post", zhCN);
        char* content_str = cJSON_PrintUnformatted(contentObj);
        cJSON_AddStringToObject(json, "content", content_str);
        free(content_str);
        cJSON_Delete(contentObj);
    }

    char* json_str = cJSON_PrintUnformatted(json);
    log_debug("[Feishu Debug] Sending Payload: %s", json_str);

    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        log_error("[Feishu] Failed to connect for sending message");
        free(json_str);
        cJSON_Delete(json);
        mg_mgr_free(&mgr);
        free(chunk.memory);
        return;
    }

    struct mg_str host = mg_url_host(url);

    struct mg_tls_opts opts = {0};
    opts.ca = mg_str("");
    opts.name = host;
    opts.skip_verification = true;
    if (mg_url_is_ssl(url)) {
        mg_tls_init(c, &opts);
    }

    mg_printf(c,
        "POST %s HTTP/1.0\r\n"
        "Host: %.*s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        mg_url_uri(url),
        (int)host.len, host.buf,
        data->access_token ? data->access_token : "",
        (int)strlen(json_str),
        json_str
    );

    while (!chunk.done) mg_mgr_poll(&mgr, 1000);

    if (chunk.size > 0) {
        log_debug("[Feishu Debug] Response: %s", chunk.memory);
        cJSON* resp = cJSON_Parse(chunk.memory);
        if (resp) {
            cJSON* code = cJSON_GetObjectItem(resp, "code");
            if (code && code->valueint != 0) {
                cJSON* msg_item = cJSON_GetObjectItem(resp, "msg");
                log_error("[Feishu] API Error: %d - %s",
                        code->valueint, msg_item ? msg_item->valuestring : "Unknown");
                 if (code->valueint == 99991668 || code->valueint == 99991663) {
                    if (data->access_token) {
                        free(data->access_token);
                        data->access_token = NULL;
                    }
                }
            } else {
                log_info("[Feishu] Sent to %s", msg->chat_id.data);
            }
            cJSON_Delete(resp);
        }
    } else {
        log_error("[Feishu] Send failed: Empty response");
    }

    free(chunk.memory);
    cJSON_Delete(json);
    free(json_str);
    mg_mgr_free(&mgr);
}

static void feishu_destroy(Channel* self) {
    if (self->user_data) {
        FeishuChannelData* data = (FeishuChannelData*)self->user_data;
        if (data->access_token) free(data->access_token);
        if (data->app_id) free(data->app_id);
        if (data->app_secret) free(data->app_secret);
        if (data->ws) feishu_ws_destroy(data->ws);
        free(data);
    }
    free(self);
}

// =============================================================================
// Channel Factory
// =============================================================================

static Channel* feishu_channel_create(void) {
    Channel* channel = calloc(1, sizeof(Channel));
    if (!channel) return NULL;

    channel->name = strdup("feishu");
    channel->init = feishu_init;
    channel->start = feishu_start;
    channel->stop = feishu_stop;
    channel->send = feishu_send;
    channel->destroy = feishu_destroy;
    channel->user_data = NULL;
    channel->plugin_ref = NULL;

    log_info("[FeishuChannel] Created new channel instance");
    return channel;
}

// =============================================================================
// Plugin Initialization
// =============================================================================

PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context) {
    (void)context;

    log_info("[Plugin:feishu_channel] Initializing feishu channel plugin");

    // Check if this plugin is enabled in config
    if (manager && manager->config) {
        PluginConfig* pc = config_get_plugin_config(manager->config, "feishu_channel");
        if (pc && pc->config) {
            cJSON* enabled_item = cJSON_GetObjectItem(pc->config, "enabled");
            if (cJSON_IsBool(enabled_item) && !cJSON_IsTrue(enabled_item)) {
                log_info("[Plugin:feishu_channel] Plugin is disabled in config, skipping registration");
                return 0;  // Return success but don't register
            }
        } else {
            // No config found means default is disabled
            log_info("[Plugin:feishu_channel] No config found, default to disabled, skipping registration");
            return 0;
        }
    }

    // Register the channel factory
    int ret = plugin_register_channel(manager, NULL, "feishu", feishu_channel_create);

    if (ret == 0) {
        log_info("[Plugin:feishu_channel] Successfully registered feishu channel");
    } else {
        log_error("[Plugin:feishu_channel] Failed to register feishu channel");
        return -1;
    }

    return 0;
}

PLUGIN_EXPORT int plugin_cleanup(void) {
    log_info("[Plugin:feishu_channel] Cleaning up feishu channel plugin");
    return 0;
}

// =============================================================================
// Plugin Information
// =============================================================================

static PluginInfo g_plugin_info = {
    .version = 1,
    .type = PLUGIN_CHANNEL,
    .name = "feishu_channel",
    .description = "Feishu (Lark) bot channel for Primagen",
    .plugin_id = "feishu_channel_bot"
};

PLUGIN_EXPORT PluginInfo* plugin_get_info(void) {
    return &g_plugin_info;
}
