/*
 * Slack Channel Plugin
 *
 * A plugin that provides Slack channel support for Primagen.
 * Uses Slack API for sending messages.
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
#include <stdint.h>

// =============================================================================
// Channel Data
// =============================================================================

typedef struct {
    MessageBus* bus;
    bool running;
    char* bot_token;
} SlackChannelData;

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

static bool slack_init(Channel* self, Config* config, MessageBus* bus) {
    SlackChannelData* data = malloc(sizeof(SlackChannelData));
    data->bus = bus;
    data->running = false;
    data->bot_token = NULL;
    self->user_data = data;

    // Get plugin configuration
    PluginConfig* plugin_cfg = config_get_plugin_config(config, "slack_channel");
    if (plugin_cfg && plugin_cfg->config) {
        cJSON* bot_token = cJSON_GetObjectItem(plugin_cfg->config, "bot_token");

        data->bot_token = bot_token && cJSON_IsString(bot_token) ? strdup(bot_token->valuestring) : strdup("");
    }

    log_info("[SlackChannel] Initialized");
    return true;
}

static void slack_start(Channel* self) {
    SlackChannelData* data = (SlackChannelData*)self->user_data;
    if (!data->bot_token || strlen(data->bot_token) == 0) return;

    log_info("[Slack] Starting channel");
    log_info("[Slack] Note: Socket Mode receiving is not yet implemented in C port.");
    data->running = true;
}

static void slack_stop(Channel* self) {
    SlackChannelData* data = (SlackChannelData*)self->user_data;
    data->running = false;
}

static void slack_send(Channel* self, OutboundMessage* msg) {
    SlackChannelData* data = (SlackChannelData*)self->user_data;
    if (!data->bot_token || strlen(data->bot_token) == 0) return;

    // Only process messages meant for this channel
    if (strcmp(msg->channel.data, "slack") != 0) return;

    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';

    mg_mgr_init(&mgr);

    char url[] = "https://slack.com/api/chat.postMessage";

    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "channel", msg->chat_id.data);
    cJSON_AddStringToObject(json, "text", msg->content.data);
    char* json_str = cJSON_PrintUnformatted(json);

    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        log_error("[Slack] Send failed: connection error");
        free(json_str);
        cJSON_Delete(json);
        mg_mgr_free(&mgr);
        free(chunk.memory);
        return;
    }

    struct mg_str host = mg_url_host(url);
    mg_printf(c,
        "POST %s HTTP/1.0\n"
        "Host: %.*s\n"
        "Authorization: Bearer %s\n"
        "Content-Type: application/json; charset=utf-8\n"
        "Content-Length: %d\n"
        "\n"
        "%s",
        mg_url_uri(url),
        (int)host.len, host.buf,
        data->bot_token,
        (int)strlen(json_str),
        json_str
    );

    while (!chunk.done) mg_mgr_poll(&mgr, 1000);

    if (chunk.size > 0) {
        log_info("[Slack] Sent to %s", msg->chat_id.data);
    } else {
        log_error("[Slack] Send failed: empty response");
    }

    cJSON_Delete(json);
    free(json_str);
    mg_mgr_free(&mgr);
    free(chunk.memory);
}

static void slack_destroy(Channel* self) {
    if (self->user_data) free(self->user_data);
    log_info("[SlackChannel] Destroyed");
    free(self);
}

// =============================================================================
// Channel Factory
// =============================================================================

static Channel* slack_channel_create(void) {
    Channel* channel = calloc(1, sizeof(Channel));
    if (!channel) return NULL;

    channel->name = strdup("slack");
    channel->init = slack_init;
    channel->start = slack_start;
    channel->stop = slack_stop;
    channel->send = slack_send;
    channel->destroy = slack_destroy;
    channel->user_data = NULL;
    channel->plugin_ref = NULL;

    log_info("[SlackChannel] Created new channel instance");
    return channel;
}

// =============================================================================
// Plugin Initialization
// =============================================================================

PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context) {
    (void)context;

    log_info("[Plugin:slack_channel] Initializing slack channel plugin");

    // Register the channel factory
    int ret = plugin_register_channel(manager, NULL, "slack", slack_channel_create);

    if (ret == 0) {
        log_info("[Plugin:slack_channel] Successfully registered slack channel");
    } else {
        log_error("[Plugin:slack_channel] Failed to register slack channel");
        return -1;
    }

    return 0;
}

PLUGIN_EXPORT int plugin_cleanup(void) {
    log_info("[Plugin:slack_channel] Cleaning up slack channel plugin");
    return 0;
}

// =============================================================================
// Plugin Information
// =============================================================================

static PluginInfo g_plugin_info = {
    .version = 1,
    .type = PLUGIN_CHANNEL,
    .name = "slack_channel",
    .description = "Slack channel support for Primagen",
    .plugin_id = "slack_channel"
};

PLUGIN_EXPORT PluginInfo* plugin_get_info(void) {
    return &g_plugin_info;
}
