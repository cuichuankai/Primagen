/*
 * Discord Channel Plugin
 *
 * A plugin that provides Discord channel support for Primagen.
 * Uses Discord API for sending messages.
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

// =============================================================================
// Channel Data
// =============================================================================

typedef struct {
    DiscordChannelConfig* config;
    MessageBus* bus;
    bool running;
} DiscordChannelData;

struct MemoryStruct {
    char *memory;
    size_t size;
    bool done;
};

// =============================================================================
// HTTP Callback for Mongoose
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

// =============================================================================
// Channel Implementation
// =============================================================================

static bool discord_init(Channel* self, Config* config, MessageBus* bus) {
    DiscordChannelData* data = malloc(sizeof(DiscordChannelData));
    data->config = &config->channels.discord;
    data->bus = bus;
    data->running = false;
    self->user_data = data;

    log_info("[DiscordChannel] Initialized");
    return true;
}

static void discord_start(Channel* self) {
    DiscordChannelData* data = (DiscordChannelData*)self->user_data;
    if (!data->config->enabled) return;

    log_info("[Discord] Starting channel (Gateway URL: %s)", data->config->gateway_url);
    log_info("[Discord] Note: WebSocket receiving is not yet implemented in C port.");
    data->running = true;
}

static void discord_stop(Channel* self) {
    DiscordChannelData* data = (DiscordChannelData*)self->user_data;
    data->running = false;
}

static void discord_send(Channel* self, OutboundMessage* msg) {
    DiscordChannelData* data = (DiscordChannelData*)self->user_data;
    if (!data->config->enabled) return;

    // Only process messages meant for this channel
    if (strcmp(msg->channel.data, "discord") != 0) return;

    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';

    mg_mgr_init(&mgr);

    char url[512];
    snprintf(url, sizeof(url), "https://discord.com/api/v10/channels/%s/messages", msg->chat_id.data);

    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "content", msg->content.data);
    char* json_str = cJSON_PrintUnformatted(json);

    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        log_error("[Discord] Send failed: connection error");
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
        "Authorization: Bot %s\r\n"
        "Content-Type: application/json\r\n"
        "User-Agent: Primagen/1.0\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        mg_url_uri(url),
        (int)host.len, host.buf,
        data->config->token,
        (int)strlen(json_str),
        json_str
    );

    while (!chunk.done) mg_mgr_poll(&mgr, 1000);

    if (chunk.size > 0) {
        log_info("[Discord] Sent to %s", msg->chat_id.data);
    } else {
        log_error("[Discord] Send failed: empty response");
    }

    cJSON_Delete(json);
    free(json_str);
    mg_mgr_free(&mgr);
    free(chunk.memory);
}

static void discord_destroy(Channel* self) {
    if (self->user_data) free(self->user_data);
    log_info("[DiscordChannel] Destroyed");
    free(self);
}

// =============================================================================
// Channel Factory
// =============================================================================

static Channel* discord_channel_create(void) {
    Channel* channel = calloc(1, sizeof(Channel));
    if (!channel) return NULL;

    channel->name = strdup("discord");
    channel->init = discord_init;
    channel->start = discord_start;
    channel->stop = discord_stop;
    channel->send = discord_send;
    channel->destroy = discord_destroy;
    channel->user_data = NULL;
    channel->plugin_ref = NULL;

    log_info("[DiscordChannel] Created new channel instance");
    return channel;
}

// =============================================================================
// Plugin Initialization
// =============================================================================

PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context) {
    (void)context;

    log_info("[Plugin:discord_channel] Initializing discord channel plugin");

    // Register the channel factory
    int ret = plugin_register_channel(manager, NULL, "discord", discord_channel_create);

    if (ret == 0) {
        log_info("[Plugin:discord_channel] Successfully registered discord channel");
    } else {
        log_error("[Plugin:discord_channel] Failed to register discord channel");
        return -1;
    }

    return 0;
}

PLUGIN_EXPORT int plugin_cleanup(void) {
    log_info("[Plugin:discord_channel] Cleaning up discord channel plugin");
    return 0;
}

// =============================================================================
// Plugin Information
// =============================================================================

static PluginInfo g_plugin_info = {
    .version = 1,
    .type = PLUGIN_CHANNEL,
    .name = "discord_channel",
    .description = "Discord channel support for Primagen",
    .plugin_id = "discord_channel"
};

PLUGIN_EXPORT PluginInfo* plugin_get_info(void) {
    return &g_plugin_info;
}
