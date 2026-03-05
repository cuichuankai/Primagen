#include "../include/channel.h"
#include "../include/config.h"
#include "../bus/message_bus.h"
#include "../include/message.h"
#include "../vendor/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

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
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(!ptr) return 0;
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    return realsize;
}

static void refresh_token(DingTalkChannelData* data) {
    CURL* curl = curl_easy_init();
    if (curl) {
        struct MemoryStruct chunk;
        chunk.memory = malloc(1);
        chunk.size = 0;

        cJSON* payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "appKey", data->config->client_id);
        cJSON_AddStringToObject(payload, "appSecret", data->config->client_secret);
        char* json_str = cJSON_PrintUnformatted(payload);

        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, "https://api.dingtalk.com/v1.0/oauth2/accessToken");
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            cJSON* resp = cJSON_Parse(chunk.memory);
            if (resp) {
                cJSON* token = cJSON_GetObjectItem(resp, "accessToken");
                if (cJSON_IsString(token)) {
                    if (data->access_token) free(data->access_token);
                    data->access_token = strdup(token->valuestring);
                    // expireIn is usually 7200
                    data->token_expiry = 7200; // Simplified
                }
                cJSON_Delete(resp);
            }
        }
        
        free(chunk.memory);
        free(json_str);
        cJSON_Delete(payload);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
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

    CURL* curl = curl_easy_init();
    if (curl) {
        char url[512];
        // Note: Using batchSend for robot
        snprintf(url, sizeof(url), "https://api.dingtalk.com/v1.0/robot/oToMessages/batchSend");
        
        struct curl_slist* headers = NULL;
        char token_header[256];
        snprintf(token_header, sizeof(token_header), "x-acs-dingtalk-access-token: %s", data->access_token);
        headers = curl_slist_append(headers, token_header);
        headers = curl_slist_append(headers, "Content-Type: application/json");

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

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            fprintf(stderr, "[DingTalk] Send failed: %s\n", curl_easy_strerror(res));
        } else {
            printf("[DingTalk] Sent to %s\n", msg->chat_id.data);
        }

        cJSON_Delete(json);
        free(json_str);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
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
