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
    DiscordChannelConfig* config;
    MessageBus* bus;
    bool running;
} DiscordChannelData;

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

    CURL* curl = curl_easy_init();
    if (curl) {
        char url[512];
        snprintf(url, sizeof(url), "https://discord.com/api/v10/channels/%s/messages", msg->chat_id.data);
        
        struct curl_slist* headers = NULL;
        char auth_header[256];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bot %s", data->config->token);
        headers = curl_slist_append(headers, auth_header);
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "User-Agent: Nanobot/1.0");

        cJSON* json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "content", msg->content.data);
        char* json_str = cJSON_PrintUnformatted(json);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            fprintf(stderr, "[Discord] Send failed: %s\n", curl_easy_strerror(res));
        } else {
            printf("[Discord] Sent to %s\n", msg->chat_id.data);
        }

        cJSON_Delete(json);
        free(json_str);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
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
