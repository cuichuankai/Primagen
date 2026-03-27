/*
 * Telegram Channel Plugin
 *
 * A plugin that provides Telegram channel support for Primagen.
 * Uses Telegram Bot API for sending and receiving messages.
 *
 * Build: make
 * Install: make install
 */

#include "../../../src/include/channel.h"
#include "../../../src/include/config.h"
#include "../../../src/include/message.h"
#include "../../../src/bus/message_bus.h"
#include "../../../src/vendor/cJSON/cJSON.h"
#include "../../../src/vendor/mongoose/mongoose.h"
#include "../../../src/include/logger.h"
#include "../../../src/plugin/plugin_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>

// =============================================================================
// Channel Data
// =============================================================================

typedef struct {
    MessageBus* bus;
    pthread_t thread_id;
    bool running;
    long last_update_id;
    char* token;
    char* dns4;
    char* dns6;
    int dns_timeout_ms;
} TelegramChannelData;

struct MemoryStruct {
    char *memory;
    size_t size;
    bool done;
};

static bool memory_append_chunk(struct MemoryStruct* ms, const char* data, size_t len) {
    if (!ms) return false;
    if (len > 0 && !data) return false;
    if (len > SIZE_MAX - ms->size - 1) return false;
    size_t new_size = ms->size + len;
    char* new_mem = realloc(ms->memory, new_size + 1);
    if (!new_mem) return false;
    ms->memory = new_mem;
    if (len > 0) {
        memcpy(ms->memory + ms->size, data, len);
    }
    ms->size = new_size;
    ms->memory[ms->size] = '\0';
    return true;
}

static void apply_dns_config(struct mg_mgr* mgr, const TelegramChannelData* data) {
    if (!mgr || !data) return;
    if (data->dns4 && data->dns4[0]) mgr->dns4.url = data->dns4;
    if (data->dns6 && data->dns6[0]) mgr->dns6.url = data->dns6;
    if (data->dns_timeout_ms > 0) mgr->dnstimeout = data->dns_timeout_ms;
}

// =============================================================================
// HTTP Callback for Mongoose
// =============================================================================

static void fn(struct mg_connection *c, int ev, void *ev_data) {
    struct MemoryStruct *ms = (struct MemoryStruct *) c->fn_data;
    if (!ms) return;
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        if (!memory_append_chunk(ms, hm->body.buf, hm->body.len)) {
            ms->done = true;
            c->is_closing = 1;
            return;
        }
        c->is_closing = 1;
        ms->done = true;
    } else if (ev == MG_EV_ERROR) {
        ms->done = true;
    }
}

// =============================================================================
// Channel Implementation
// =============================================================================

static void* telegram_poller(void* arg) {
    Channel* self = (Channel*)arg;
    TelegramChannelData* data = (TelegramChannelData*)self->user_data;

    log_info("[Telegram] Polling started...");

    while (data->running) {
        struct mg_mgr mgr;
        struct MemoryStruct chunk = {0};
        chunk.memory = malloc(1);
        chunk.memory[0] = '\0';

        mg_mgr_init(&mgr);
        apply_dns_config(&mgr, data);

        char url[512];
        snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/getUpdates?offset=%ld&timeout=30",
                 data->token, data->last_update_id + 1);

        struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
        if (c) {
            struct mg_str host = mg_url_host(url);
            mg_printf(c,
                "GET %s HTTP/1.0\r\n"
                "Host: %.*s\r\n"
                "\r\n",
                mg_url_uri(url),
                (int)host.len, host.buf
            );

            while (!chunk.done) mg_mgr_poll(&mgr, 1000);

            if (chunk.size > 0) {
                cJSON *json = cJSON_Parse(chunk.memory);
                if (json) {
                    cJSON *result = cJSON_GetObjectItem(json, "result");
                    if (cJSON_IsArray(result)) {
                        cJSON *item;
                        cJSON_ArrayForEach(item, result) {
                            cJSON *update_id = cJSON_GetObjectItem(item, "update_id");
                            if (update_id) data->last_update_id = update_id->valueint;

                            cJSON *message = cJSON_GetObjectItem(item, "message");
                            if (message) {
                                cJSON *chat = cJSON_GetObjectItem(message, "chat");
                                cJSON *text = cJSON_GetObjectItem(message, "text");
                                cJSON *chat_id_json = cJSON_GetObjectItem(chat, "id");

                                if (text && chat_id_json) {
                                    char chat_id_str[64];
                                    if (cJSON_IsNumber(chat_id_json))
                                        snprintf(chat_id_str, sizeof(chat_id_str), "%lld", (long long)chat_id_json->valuedouble);
                                    else
                                        snprintf(chat_id_str, sizeof(chat_id_str), "%s", chat_id_json->valuestring);

                                    log_info("[Telegram] Received: %s from %s", text->valuestring, chat_id_str);

                                    InboundMessage* msg = inbound_message_new("telegram", chat_id_str, text->valuestring);
                                    message_bus_send_inbound(data->bus, msg);
                                }
                            }
                        }
                    }
                    cJSON_Delete(json);
                }
            }
        } else {
            log_error("[Telegram] Poll failed: connection error");
            sleep(5);
        }

        free(chunk.memory);
        mg_mgr_free(&mgr);

        usleep(100000);
    }
    return NULL;
}

static bool telegram_init(Channel* self, Config* cfg, MessageBus* bus) {
    TelegramChannelData* data = malloc(sizeof(TelegramChannelData));
    data->bus = bus;
    data->running = false;
    data->last_update_id = 0;
    data->token = NULL;
    data->dns4 = NULL;
    data->dns6 = NULL;
    data->dns_timeout_ms = 0;
    self->user_data = data;

    // Get plugin configuration
    PluginConfig* plugin_cfg = config_get_plugin_config(cfg, "telegram_channel");
    if (plugin_cfg && plugin_cfg->config) {
        cJSON* token = cJSON_GetObjectItem(plugin_cfg->config, "token");
        data->token = token && cJSON_IsString(token) ? strdup(token->valuestring) : strdup("");
    }

    // Check if token is configured
    if (!data->token || strlen(data->token) == 0) {
        log_info("[TelegramChannel] No token configured, skipping initialization");
        return false;
    }

    DNSConfig* dns_cfg = config_get_dns_config(cfg);
    if (dns_cfg) {
        if (dns_cfg->dns4 && dns_cfg->dns4[0]) data->dns4 = strdup(dns_cfg->dns4);
        if (dns_cfg->dns6 && dns_cfg->dns6[0]) data->dns6 = strdup(dns_cfg->dns6);
        if (dns_cfg->dns_timeout_ms > 0) data->dns_timeout_ms = dns_cfg->dns_timeout_ms;
    }

    log_info("[TelegramChannel] Initialized with token");
    return true;
}

static void telegram_start(Channel* self) {
    TelegramChannelData* data = (TelegramChannelData*)self->user_data;
    data->running = true;
    pthread_create(&data->thread_id, NULL, telegram_poller, self);
    log_info("[TelegramChannel] Started");
}

static void telegram_stop(Channel* self) {
    TelegramChannelData* data = (TelegramChannelData*)self->user_data;
    if (data) {
        data->running = false;
        pthread_join(data->thread_id, NULL);
    }
    log_info("[TelegramChannel] Stopped");
}

static void telegram_send(Channel* self, OutboundMessage* msg) {
    TelegramChannelData* data = (TelegramChannelData*)self->user_data;
    if (!data) return;

    // Only process messages meant for this channel
    if (strcmp(msg->channel.data, "telegram") != 0) return;

    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';

    mg_mgr_init(&mgr);
    apply_dns_config(&mgr, data);

    char url[512];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", data->token);

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "chat_id", msg->chat_id.data);
    cJSON_AddStringToObject(json, "text", msg->content.data);
    char *json_str = cJSON_PrintUnformatted(json);

    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        log_error("[Telegram] Send failed: connection error");
        free(json_str);
        cJSON_Delete(json);
        mg_mgr_free(&mgr);
        free(chunk.memory);
        return;
    }

    struct mg_str host = mg_url_host(url);
    mg_printf(c,
        "POST %s HTTP/1.0\r\n"
        "Host: %.*s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        mg_url_uri(url),
        (int)host.len, host.buf,
        (int)strlen(json_str),
        json_str
    );

    while (!chunk.done) mg_mgr_poll(&mgr, 1000);

    if (chunk.size == 0) {
        log_error("[Telegram] Send failed: empty response");
    } else {
        log_info("[Telegram] Sent message to %s", msg->chat_id.data);
    }

    free(json_str);
    cJSON_Delete(json);
    mg_mgr_free(&mgr);
    free(chunk.memory);
}

static void telegram_destroy(Channel* self) {
    TelegramChannelData* data = (TelegramChannelData*)self->user_data;
    if (data) {
        free(data->token);
        free(data->dns4);
        free(data->dns6);
        free(data);
    }
    log_info("[TelegramChannel] Destroyed");
    free(self);
}

// =============================================================================
// Channel Factory
// =============================================================================

static Channel* telegram_channel_create(void) {
    Channel* channel = calloc(1, sizeof(Channel));
    if (!channel) return NULL;

    channel->name = strdup("telegram");
    channel->init = telegram_init;
    channel->start = telegram_start;
    channel->stop = telegram_stop;
    channel->send = telegram_send;
    channel->destroy = telegram_destroy;
    channel->user_data = NULL;
    channel->plugin_ref = NULL;

    log_info("[TelegramChannel] Created new channel instance");
    return channel;
}

// =============================================================================
// Plugin Initialization
// =============================================================================

PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context) {
    (void)context;

    log_info("[Plugin:telegram_channel] Initializing telegram channel plugin");

    // Register the channel factory
    int ret = plugin_register_channel(manager, NULL, "telegram", telegram_channel_create);

    if (ret == 0) {
        log_info("[Plugin:telegram_channel] Successfully registered telegram channel");
    } else {
        log_error("[Plugin:telegram_channel] Failed to register telegram channel");
        return -1;
    }

    return 0;
}

PLUGIN_EXPORT int plugin_cleanup(void) {
    log_info("[Plugin:telegram_channel] Cleaning up telegram channel plugin");
    return 0;
}

// =============================================================================
// Plugin Information
// =============================================================================

static PluginInfo g_plugin_info = {
    .version = 1,
    .type = PLUGIN_CHANNEL,
    .name = "telegram_channel",
    .description = "Telegram channel support for Primagen",
    .plugin_id = "telegram_channel"
};

PLUGIN_EXPORT PluginInfo* plugin_get_info(void) {
    return &g_plugin_info;
}
