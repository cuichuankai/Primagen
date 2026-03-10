#include "../include/channel.h"
#include "../include/config.h"
#include "../bus/message_bus.h"
#include "../include/message.h"
#include "../vendor/cJSON/cJSON.h"
#include "../vendor/mongoose/mongoose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static bool discord_init(Channel* self, Config* config, MessageBus* bus) {
    DiscordChannelData* data = malloc(sizeof(DiscordChannelData));
    data->config = &config->channels.discord;
    data->bus = bus;
    data->running = false;
    self->user_data = data;
    return true;
}

static void discord_start(Channel* self) {
    DiscordChannelData* data = (DiscordChannelData*)self->user_data;
    if (!data->config->enabled) return;
    printf("[Discord] Starting channel (Gateway URL: %s)...\n", data->config->gateway_url);
    printf("[Discord] Note: WebSocket receiving is not yet implemented in C port.\n");
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
        fprintf(stderr, "[Discord] Send failed: connection error\n");
        free(json_str);
        cJSON_Delete(json);
        mg_mgr_free(&mgr);
        free(chunk.memory);
        return;
    }
    
    // struct mg_tls_opts opts = {0};
    // opts.ca = mg_str("ca.pem");

    struct mg_str host = mg_url_host(url);
    mg_printf(c, 
        "POST %s HTTP/1.0\r\n"
        "Host: %.*s\r\n"
        "Authorization: Bot %s\r\n"
        "Content-Type: application/json\r\n"
        "User-Agent: Nanobot/1.0\r\n"
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
        printf("[Discord] Sent to %s\n", msg->chat_id.data);
    } else {
        fprintf(stderr, "[Discord] Send failed: empty response\n");
    }

    cJSON_Delete(json);
    free(json_str);
    mg_mgr_free(&mgr);
    free(chunk.memory);
}

static void discord_destroy(Channel* self) {
    if (self->user_data) free(self->user_data);
    free(self);
}

Channel* channel_create_discord() {
    Channel* channel = malloc(sizeof(Channel));
    channel->name = strdup("discord");
    channel->init = discord_init;
    channel->start = discord_start;
    channel->stop = discord_stop;
    channel->send = discord_send;
    channel->destroy = discord_destroy;
    channel->user_data = NULL;
    return channel;
}
