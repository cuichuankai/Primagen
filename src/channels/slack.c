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
    SlackChannelConfig* config;
    MessageBus* bus;
    bool running;
} SlackChannelData;

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

static bool slack_init(Channel* self, Config* config, MessageBus* bus) {
    SlackChannelData* data = malloc(sizeof(SlackChannelData));
    data->config = &config->channels.slack;
    data->bus = bus;
    data->running = false;
    self->user_data = data;
    return true;
}

static void slack_start(Channel* self) {
    SlackChannelData* data = (SlackChannelData*)self->user_data;
    if (!data->config->enabled) return;
    printf("[Slack] Starting channel (Mode: %s)...\n", data->config->mode);
    printf("[Slack] Note: Socket Mode receiving is not yet implemented in C port.\n");
    data->running = true;
}

static void slack_stop(Channel* self) {
    SlackChannelData* data = (SlackChannelData*)self->user_data;
    data->running = false;
}

static void slack_send(Channel* self, OutboundMessage* msg) {
    SlackChannelData* data = (SlackChannelData*)self->user_data;
    if (!data->config->enabled) return;
    
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
        fprintf(stderr, "[Slack] Send failed: connection error\n");
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
        "Authorization: Bearer %s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        mg_url_uri(url), 
        (int)host.len, host.buf,
        data->config->bot_token,
        (int)strlen(json_str),
        json_str
    );

    while (!chunk.done) mg_mgr_poll(&mgr, 1000);
    
    if (chunk.size > 0) {
        printf("[Slack] Sent to %s\n", msg->chat_id.data);
    } else {
        fprintf(stderr, "[Slack] Send failed: empty response\n");
    }

    cJSON_Delete(json);
    free(json_str);
    mg_mgr_free(&mgr);
    free(chunk.memory);
}

static void slack_destroy(Channel* self) {
    if (self->user_data) free(self->user_data);
    free(self);
}

Channel* channel_create_slack() {
    Channel* channel = malloc(sizeof(Channel));
    channel->name = strdup("slack");
    channel->init = slack_init;
    channel->start = slack_start;
    channel->stop = slack_stop;
    channel->send = slack_send;
    channel->destroy = slack_destroy;
    channel->user_data = NULL;
    return channel;
}
