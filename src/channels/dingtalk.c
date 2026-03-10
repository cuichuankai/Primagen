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
    DingTalkChannelConfig* config;
    MessageBus* bus;
    bool running;
    char* access_token;
    long token_expiry;
} DingTalkChannelData;

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

static void refresh_token(DingTalkChannelData* data) {
    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';
    
    mg_mgr_init(&mgr);

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "appKey", data->config->client_id);
    cJSON_AddStringToObject(payload, "appSecret", data->config->client_secret);
    char* json_str = cJSON_PrintUnformatted(payload);
    
    const char* url = "https://api.dingtalk.com/v1.0/oauth2/accessToken";
    
    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        free(json_str);
        cJSON_Delete(payload);
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
    
    if (chunk.size > 0) {
        cJSON* resp = cJSON_Parse(chunk.memory);
        if (resp) {
            cJSON* token = cJSON_GetObjectItem(resp, "accessToken");
            if (cJSON_IsString(token)) {
                if (data->access_token) free(data->access_token);
                data->access_token = strdup(token->valuestring);
                data->token_expiry = 7200; 
            }
            cJSON_Delete(resp);
        }
    }
    
    free(chunk.memory);
    free(json_str);
    cJSON_Delete(payload);
    mg_mgr_free(&mgr);
}

static bool dingtalk_init(Channel* self, Config* config, MessageBus* bus) {
    DingTalkChannelData* data = malloc(sizeof(DingTalkChannelData));
    data->config = &config->channels.dingtalk;
    data->bus = bus;
    data->running = false;
    data->access_token = NULL;
    data->token_expiry = 0;
    self->user_data = data;
    return true;
}

static void dingtalk_start(Channel* self) {
    DingTalkChannelData* data = (DingTalkChannelData*)self->user_data;
    if (!data->config->enabled) return;
    printf("[DingTalk] Starting channel...\n");
    printf("[DingTalk] Note: Stream Mode receiving is not yet implemented in C port.\n");
    data->running = true;
    refresh_token(data);
}

static void dingtalk_stop(Channel* self) {
    DingTalkChannelData* data = (DingTalkChannelData*)self->user_data;
    data->running = false;
}

static void dingtalk_send(Channel* self, OutboundMessage* msg) {
    DingTalkChannelData* data = (DingTalkChannelData*)self->user_data;
    if (!data->config->enabled) return;
    
    // Only process messages meant for this channel
    if (strcmp(msg->channel.data, "dingtalk") != 0) return;

    if (!data->access_token) refresh_token(data);

    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';
    
    mg_mgr_init(&mgr);

    char url[512];
    snprintf(url, sizeof(url), "https://api.dingtalk.com/v1.0/robot/oToMessages/batchSend");
    
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "robotCode", data->config->client_id);
    
    cJSON* userIds = cJSON_CreateArray();
    cJSON_AddItemToArray(userIds, cJSON_CreateString(msg->chat_id.data));
    cJSON_AddItemToObject(json, "userIds", userIds);
    
    cJSON_AddStringToObject(json, "msgKey", "sampleMarkdown");
    
    cJSON* msgParam = cJSON_CreateObject();
    cJSON_AddStringToObject(msgParam, "text", msg->content.data);
    cJSON_AddStringToObject(msgParam, "title", "Nanobot Reply");
    char* param_str = cJSON_PrintUnformatted(msgParam);
    cJSON_AddStringToObject(json, "msgParam", param_str);
    free(param_str);
    cJSON_Delete(msgParam);

    char* json_str = cJSON_PrintUnformatted(json);

    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        fprintf(stderr, "[DingTalk] Send failed: connection error\n");
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
        "x-acs-dingtalk-access-token: %s\r\n"
        "Content-Type: application/json\r\n"
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
        printf("[DingTalk] Sent to %s\n", msg->chat_id.data);
    } else {
        fprintf(stderr, "[DingTalk] Send failed: empty response\n");
    }

    cJSON_Delete(json);
    free(json_str);
    mg_mgr_free(&mgr);
    free(chunk.memory);
}

static void dingtalk_destroy(Channel* self) {
    DingTalkChannelData* data = (DingTalkChannelData*)self->user_data;
    if (data) {
        if (data->access_token) free(data->access_token);
        free(data);
    }
    free(self);
}

Channel* channel_create_dingtalk() {
    Channel* channel = malloc(sizeof(Channel));
    channel->name = strdup("dingtalk");
    channel->init = dingtalk_init;
    channel->start = dingtalk_start;
    channel->stop = dingtalk_stop;
    channel->send = dingtalk_send;
    channel->destroy = dingtalk_destroy;
    channel->user_data = NULL;
    return channel;
}
