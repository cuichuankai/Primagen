#include "../include/channel.h"
#include "../vendor/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <curl/curl.h>

typedef struct {
    MessageBus* bus;
    char* token;
    pthread_t thread_id;
    bool running;
    long last_update_id;
} TelegramData;

// Curl helper (reused from tools)
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

static void* telegram_poller(void* arg) {
    Channel* self = (Channel*)arg;
    TelegramData* data = (TelegramData*)self->user_data;
    
    printf("[Telegram] Polling started...\n");
    
    while (data->running) {
        CURL *curl = curl_easy_init();
        if (curl) {
            char url[512];
            snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/getUpdates?offset=%ld&timeout=30", 
                     data->token, data->last_update_id + 1);
            
            struct MemoryStruct chunk;
            chunk.memory = malloc(1);
            chunk.size = 0;
            
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
            
            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) {
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
                                    // Handle int or string chat_id
                                    if (cJSON_IsNumber(chat_id_json)) 
                                        snprintf(chat_id_str, sizeof(chat_id_str), "%lld", (long long)chat_id_json->valuedouble);
                                    else
                                        snprintf(chat_id_str, sizeof(chat_id_str), "%s", chat_id_json->valuestring);
                                        
                                    printf("[Telegram] Received: %s from %s\n", text->valuestring, chat_id_str);
                                    
                                    InboundMessage* msg = inbound_message_new("telegram", chat_id_str, text->valuestring);
                                    message_bus_send_inbound(data->bus, msg);
                                }
                            }
                        }
                    }
                    cJSON_Delete(json);
                }
            } else {
                fprintf(stderr, "[Telegram] Poll failed: %s\n", curl_easy_strerror(res));
                sleep(5); // Backoff
            }
            
            free(chunk.memory);
            curl_easy_cleanup(curl);
        }
        usleep(100000); // Small pause
    }
    return NULL;
}

static bool telegram_init(Channel* self, Config* cfg, MessageBus* bus) {
    if (!cfg->channels.telegram.enabled || strlen(cfg->channels.telegram.token) == 0) {
        return false;
    }
    
    TelegramData* data = malloc(sizeof(TelegramData));
    data->bus = bus;
    data->token = strdup(cfg->channels.telegram.token);
    data->running = false;
    data->last_update_id = 0;
    self->user_data = data;
    return true;
}

static void telegram_start(Channel* self) {
    TelegramData* data = (TelegramData*)self->user_data;
    data->running = true;
    pthread_create(&data->thread_id, NULL, telegram_poller, self);
}

static void telegram_stop(Channel* self) {
    TelegramData* data = (TelegramData*)self->user_data;
    if (data) {
        data->running = false;
        pthread_join(data->thread_id, NULL);
    }
}

static void telegram_send(Channel* self, OutboundMessage* msg) {
    TelegramData* data = (TelegramData*)self->user_data;
    if (!data) return;
    
    // Only process messages meant for this channel
    if (strcmp(msg->channel.data, "telegram") != 0) return;
    
    CURL *curl = curl_easy_init();
    if (curl) {
        char url[512];
        snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", data->token);
        
        cJSON *json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "chat_id", msg->chat_id.data);
        cJSON_AddStringToObject(json, "text", msg->content.data);
        char *json_str = cJSON_PrintUnformatted(json);
        
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        // Fire and forget for now (or log error)
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            fprintf(stderr, "[Telegram] Send failed: %s\n", curl_easy_strerror(res));
        }
        
        free(json_str);
        cJSON_Delete(json);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}

static void telegram_destroy(Channel* self) {
    TelegramData* data = (TelegramData*)self->user_data;
    if (data) {
        free(data->token);
        free(data);
    }
    free(self);
}

Channel* channel_create_telegram() {
    Channel* ch = malloc(sizeof(Channel));
    ch->name = "telegram";
    ch->init = telegram_init;
    ch->start = telegram_start;
    ch->stop = telegram_stop;
    ch->send = telegram_send;
    ch->destroy = telegram_destroy;
    ch->user_data = NULL;
    return ch;
}
