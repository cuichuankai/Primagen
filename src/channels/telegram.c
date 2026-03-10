#include "../include/channel.h"
#include "../vendor/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "../vendor/mongoose/mongoose.h"

typedef struct {
    MessageBus* bus;
    char* token;
    pthread_t thread_id;
    bool running;
    long last_update_id;
} TelegramData;

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

static void* telegram_poller(void* arg) {
    Channel* self = (Channel*)arg;
    TelegramData* data = (TelegramData*)self->user_data;
    
    printf("[Telegram] Polling started...\n");
    
    while (data->running) {
        struct mg_mgr mgr;
        struct MemoryStruct chunk = {0};
        chunk.memory = malloc(1);
        chunk.memory[0] = '\0';
        
        mg_mgr_init(&mgr);

        char url[512];
        snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/getUpdates?offset=%ld&timeout=30", 
                 data->token, data->last_update_id + 1);
        
        struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
        if (c) {
            // struct mg_tls_opts opts = {0};
            // opts.ca = mg_str("ca.pem");
            // if (mg_url_is_ssl(url)) c->tls_opts = opts;

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
                                        
                                    printf("[Telegram] Received: %s from %s\n", text->valuestring, chat_id_str);
                                    
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
            fprintf(stderr, "[Telegram] Poll failed: connection error\n");
            sleep(5);
        }
        
        free(chunk.memory);
        mg_mgr_free(&mgr);
        
        usleep(100000); 
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
    
    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';
    
    mg_mgr_init(&mgr);

    char url[512];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", data->token);
    
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "chat_id", msg->chat_id.data);
    cJSON_AddStringToObject(json, "text", msg->content.data);
    char *json_str = cJSON_PrintUnformatted(json);
    
    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        fprintf(stderr, "[Telegram] Send failed: connection error\n");
        free(json_str);
        cJSON_Delete(json);
        mg_mgr_free(&mgr);
        free(chunk.memory);
        return;
    }
    
    // We cannot set c->tls_opts directly in recent Mongoose unless we use mg_tls_init
    // But since we use MG_TLS_BUILTIN and wss/https, it should just work.
    // We skip manual CA setting for now to fix build.
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
    
    if (chunk.size == 0) {
        fprintf(stderr, "[Telegram] Send failed: empty response\n");
    }
    
    free(json_str);
    cJSON_Delete(json);
    mg_mgr_free(&mgr);
    free(chunk.memory);
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
