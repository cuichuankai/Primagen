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
    SlackChannelConfig* config;
    MessageBus* bus;
    bool running;
} SlackChannelData;

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

    CURL* curl = curl_easy_init();
    if (curl) {
        char url[] = "https://slack.com/api/chat.postMessage";
        
        struct curl_slist* headers = NULL;
        char auth_header[256];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", data->config->bot_token);
        headers = curl_slist_append(headers, auth_header);
        headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");

        cJSON* json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "channel", msg->chat_id.data);
        cJSON_AddStringToObject(json, "text", msg->content.data);
        char* json_str = cJSON_PrintUnformatted(json);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            fprintf(stderr, "[Slack] Send failed: %s\n", curl_easy_strerror(res));
        } else {
            printf("[Slack] Sent to %s\n", msg->chat_id.data);
        }

        cJSON_Delete(json);
        free(json_str);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
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
