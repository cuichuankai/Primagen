#include "../include/channel.h"
#include "../include/config.h"
#include "../bus/message_bus.h"
#include "../include/message.h"
#include "../include/logger.h"
#include "../vendor/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <unistd.h>
#include <pthread.h>

#include "feishu_ws.h"

typedef struct {
    FeishuChannelConfig* config;
    MessageBus* bus;
    bool running;
    char* access_token;
    pthread_t thread_id;
    FeishuWS* ws;
} FeishuChannelData;

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

static void refresh_token(FeishuChannelData* data) {
    CURL* curl = curl_easy_init();
    if (curl) {
        struct MemoryStruct chunk;
        chunk.memory = malloc(1);
        chunk.size = 0;

        cJSON* payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "app_id", data->config->app_id);
        cJSON_AddStringToObject(payload, "app_secret", data->config->app_secret);
        char* json_str = cJSON_PrintUnformatted(payload);

        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");

        curl_easy_setopt(curl, CURLOPT_URL, "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal");
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            cJSON* resp = cJSON_Parse(chunk.memory);
            if (resp) {
                // Check for error code
                cJSON* code = cJSON_GetObjectItem(resp, "code");
                if (code && code->valueint != 0) {
                    cJSON* msg = cJSON_GetObjectItem(resp, "msg");
                    log_error("[Feishu] Token refresh failed: %d - %s", 
                            code->valueint, msg ? msg->valuestring : "Unknown error");
                } else {
                    cJSON* token = cJSON_GetObjectItem(resp, "tenant_access_token");
                    if (cJSON_IsString(token)) {
                        if (data->access_token) free(data->access_token);
                        data->access_token = strdup(token->valuestring);
                        // log_info("[Feishu] Token refreshed successfully");
                    }
                }
                cJSON_Delete(resp);
            } else {
                log_error("[Feishu] Failed to parse token response");
            }
        } else {
            log_error("[Feishu] Token request failed: %s", curl_easy_strerror(res));
        }
        
        free(chunk.memory);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}

static char* get_ws_url(FeishuChannelData* data) {
    if (!data->access_token) refresh_token(data);
    char* ws_url = NULL;

    CURL* curl = curl_easy_init();
    if (curl) {
        struct MemoryStruct chunk;
        chunk.memory = malloc(1);
        chunk.size = 0;

        struct curl_slist* headers = NULL;
        // No Authorization header for this endpoint
        headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
        headers = curl_slist_append(headers, "locale: zh");

        cJSON* payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "AppID", data->config->app_id);
        cJSON_AddStringToObject(payload, "AppSecret", data->config->app_secret);
        char* json_str = cJSON_PrintUnformatted(payload);

        // Feishu WebSocket Endpoint
        // GenEndpointUri = "/callback/ws/endpoint"
        curl_easy_setopt(curl, CURLOPT_URL, "https://open.feishu.cn/callback/ws/endpoint");
        curl_easy_setopt(curl, CURLOPT_POST, 1L); // POST required
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str); // JSON body
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            cJSON* resp = cJSON_Parse(chunk.memory);
            if (resp) {
                cJSON* code = cJSON_GetObjectItem(resp, "code");
                if (code && code->valueint == 0) {
                    cJSON* dataObj = cJSON_GetObjectItem(resp, "data");
                    if (dataObj) {
                        cJSON* url = cJSON_GetObjectItem(dataObj, "URL"); // Case sensitive: "URL"
                        if (cJSON_IsString(url)) {
                            ws_url = strdup(url->valuestring);
                            // log_debug("[Feishu] Got WS URL: %s", ws_url);
                            
                            // Hack: Try to force V1 if possible, or check if V2 supports JSON
                            // Currently V2 is Protobuf. 
                            // Let's see if we can just replace /v2 with /v1? 
                            // (Unlikely to work if authentication differs, but worth a shot in debug)
                            // char* v2 = strstr(ws_url, "/ws/v2");
                            // if (v2) {
                            //     v2[4] = '1'; 
                            // }
                        }
                    }
                } else {
                    cJSON* msg = cJSON_GetObjectItem(resp, "msg");
                    log_error("[Feishu] Get WS URL failed: %d - %s", 
                            code ? code->valueint : -1, msg ? msg->valuestring : "Unknown");
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
    return ws_url;
}

static char* create_streaming_card(FeishuChannelData* data, const char* content) {
    if (!data->access_token) refresh_token(data);
    char* card_id = NULL;

    CURL* curl = curl_easy_init();
    if (curl) {
        struct MemoryStruct chunk;
        chunk.memory = malloc(1);
        chunk.size = 0;

        struct curl_slist* headers = NULL;
        char auth_header[256];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", data->access_token);
        headers = curl_slist_append(headers, auth_header);
        headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");

        // Construct Card JSON
        cJSON* cardData = cJSON_CreateObject();
        cJSON_AddStringToObject(cardData, "schema", "2.0");
        
        cJSON* header = cJSON_CreateObject();
        cJSON* title = cJSON_CreateObject();
        cJSON_AddStringToObject(title, "tag", "plain_text");
        cJSON_AddStringToObject(title, "content", "Primagen"); // TODO: Configurable
        cJSON_AddItemToObject(header, "title", title);
        cJSON_AddStringToObject(header, "template", "blue");
        cJSON_AddItemToObject(cardData, "header", header);

        cJSON* config = cJSON_CreateObject();
        cJSON_AddBoolToObject(config, "streaming_mode", true); // Enable streaming!
        cJSON_AddItemToObject(cardData, "config", config);

        cJSON* body = cJSON_CreateObject();
        cJSON* elements = cJSON_CreateArray();
        cJSON* element = cJSON_CreateObject();
        cJSON_AddStringToObject(element, "tag", "markdown");
        cJSON_AddStringToObject(element, "element_id", "markdown_1"); // Fixed ID for updates
        cJSON_AddStringToObject(element, "content", content);
        cJSON_AddItemToArray(elements, element);
        cJSON_AddItemToObject(body, "elements", elements);
        cJSON_AddItemToObject(cardData, "body", body);

        char* card_data_str = cJSON_PrintUnformatted(cardData);
        
        cJSON* reqBody = cJSON_CreateObject();
        cJSON_AddStringToObject(reqBody, "type", "card_json");
        cJSON_AddStringToObject(reqBody, "data", card_data_str);
        char* req_json_str = cJSON_PrintUnformatted(reqBody);

        curl_easy_setopt(curl, CURLOPT_URL, "https://open.feishu.cn/open-apis/cardkit/v1/cards");
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_json_str);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

        CURLcode res = curl_easy_perform(curl);
        cJSON* resp = NULL;
        cJSON* code = NULL;
        if (res == CURLE_OK) {
            resp = cJSON_Parse(chunk.memory);
            if (resp) {
                code = cJSON_GetObjectItem(resp, "code");
                if (code && code->valueint == 0) {
                    cJSON* dataObj = cJSON_GetObjectItem(resp, "data");
                    if (dataObj) {
                        cJSON* id = cJSON_GetObjectItem(dataObj, "card_id");
                        if (cJSON_IsString(id)) {
                            card_id = strdup(id->valuestring);
                        }
                    }
                } else {
                    cJSON* msg = cJSON_GetObjectItem(resp, "msg");
                    log_error("[Feishu] Create Card failed: %d - %s", 
                            code ? code->valueint : -1, msg ? msg->valuestring : "Unknown");
                }
            }
        }
        
        free(chunk.memory);
        free(card_data_str);
        free(req_json_str);
        cJSON_Delete(cardData);
        cJSON_Delete(reqBody);
        if (resp) cJSON_Delete(resp); // Moved delete here
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    return card_id;
}

static char* upload_image(FeishuChannelData* data, const char* filepath) {
    if (!data->access_token) refresh_token(data);
    char* image_key = NULL;
    
    CURL* curl = curl_easy_init();
    if (curl) {
        struct MemoryStruct chunk;
        chunk.memory = malloc(1);
        chunk.size = 0;

        curl_mime *mime;
        curl_mimepart *part;

        mime = curl_mime_init(curl);
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "image_type");
        curl_mime_data(part, "message", CURL_ZERO_TERMINATED);
        
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "image");
        curl_mime_filedata(part, filepath);

        struct curl_slist* headers = NULL;
        char auth_header[256];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", data->access_token);
        headers = curl_slist_append(headers, auth_header);

        curl_easy_setopt(curl, CURLOPT_URL, "https://open.feishu.cn/open-apis/im/v1/images");
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            cJSON* resp = cJSON_Parse(chunk.memory);
            if (resp) {
                cJSON* code = cJSON_GetObjectItem(resp, "code");
                if (code && code->valueint == 0) {
                    cJSON* dataObj = cJSON_GetObjectItem(resp, "data");
                    if (dataObj) {
                        cJSON* key = cJSON_GetObjectItem(dataObj, "image_key");
                        if (cJSON_IsString(key)) {
                            image_key = strdup(key->valuestring);
                        }
                    }
                } else {
                    cJSON* msg = cJSON_GetObjectItem(resp, "msg");
                    fprintf(stderr, "[Feishu] Image upload failed: %d - %s\n", 
                            code ? code->valueint : -1, msg ? msg->valuestring : "Unknown");
                }
                cJSON_Delete(resp);
            }
        }
        
        free(chunk.memory);
        curl_mime_free(mime);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    return image_key;
}

static char* upload_file(FeishuChannelData* data, const char* filepath, const char* file_type) {
    if (!data->access_token) refresh_token(data);
    char* file_key = NULL;
    
    CURL* curl = curl_easy_init();
    if (curl) {
        struct MemoryStruct chunk;
        chunk.memory = malloc(1);
        chunk.size = 0;

        curl_mime *mime;
        curl_mimepart *part;

        mime = curl_mime_init(curl);
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "file_type");
        curl_mime_data(part, file_type, CURL_ZERO_TERMINATED);
        
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "file_name");
        // Simple filename extraction
        const char* filename = strrchr(filepath, '/');
        if (filename) filename++; else filename = filepath;
        curl_mime_data(part, filename, CURL_ZERO_TERMINATED);
        
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "file");
        curl_mime_filedata(part, filepath);

        struct curl_slist* headers = NULL;
        char auth_header[256];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", data->access_token);
        headers = curl_slist_append(headers, auth_header);

        curl_easy_setopt(curl, CURLOPT_URL, "https://open.feishu.cn/open-apis/im/v1/files");
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            cJSON* resp = cJSON_Parse(chunk.memory);
            if (resp) {
                cJSON* code = cJSON_GetObjectItem(resp, "code");
                if (code && code->valueint == 0) {
                    cJSON* dataObj = cJSON_GetObjectItem(resp, "data");
                    if (dataObj) {
                        cJSON* key = cJSON_GetObjectItem(dataObj, "file_key");
                        if (cJSON_IsString(key)) {
                            file_key = strdup(key->valuestring);
                        }
                    }
                } else {
                    cJSON* msg = cJSON_GetObjectItem(resp, "msg");
                    fprintf(stderr, "[Feishu] File upload failed: %d - %s\n", 
                            code ? code->valueint : -1, msg ? msg->valuestring : "Unknown");
                }
                cJSON_Delete(resp);
            }
        }
        
        free(chunk.memory);
        curl_mime_free(mime);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    return file_key;
}

static void on_feishu_message(const char* chat_id, const char* content, const char* sender_id, void* user_data) {
    FeishuChannelData* data = (FeishuChannelData*)user_data;
    log_info("[Feishu] Received from %s: %s", sender_id, content);
    
    // Create InboundMessage
    InboundMessage* msg = inbound_message_new("feishu", chat_id, content);
    message_bus_send_inbound(data->bus, msg);
}

static void* feishu_receive_loop(void* arg) {
    FeishuChannelData* data = (FeishuChannelData*)arg;
    
    while (data->running) {
        char* url = get_ws_url(data);
        if (url) {
            log_info("[Feishu] Connecting to WebSocket...");
            data->ws = feishu_ws_create();
            if (feishu_ws_connect(data->ws, url)) {
                log_info("[Feishu] WebSocket connected.");
                feishu_ws_run(data->ws, on_feishu_message, data);
            } else {
                log_error("[Feishu] WebSocket connect failed.");
            }
            feishu_ws_destroy(data->ws);
            data->ws = NULL;
            free(url);
        }
        
        // Retry delay
        sleep(5);
    }
    return NULL;
}

static bool feishu_init(Channel* self, Config* config, MessageBus* bus) {
    FeishuChannelData* data = malloc(sizeof(FeishuChannelData));
    data->config = &config->channels.feishu;
    data->bus = bus;
    data->running = false;
    data->access_token = NULL;
    data->ws = NULL;
    self->user_data = data;
    return true;
}

static void feishu_start(Channel* self) {
    FeishuChannelData* data = (FeishuChannelData*)self->user_data;
    if (!data->config->enabled) return;
    // printf("[Feishu] Starting channel...\n");
    data->running = true;
    refresh_token(data);
    
    // Start receive thread
    pthread_create(&data->thread_id, NULL, feishu_receive_loop, data);
}

static void feishu_stop(Channel* self) {
    FeishuChannelData* data = (FeishuChannelData*)self->user_data;
    data->running = false;
    if (data->ws) feishu_ws_stop(data->ws);
    // pthread_join(data->thread_id, NULL); // Optional
}

static void feishu_send(Channel* self, OutboundMessage* msg) {
    FeishuChannelData* data = (FeishuChannelData*)self->user_data;
    if (!data->config->enabled) return;
    
    if (strcmp(msg->channel.data, "feishu") != 0) return;

    if (!data->access_token) refresh_token(data);

    if (!data->access_token) {
        log_error("[Feishu] Not connected (no token)");
        return;
    }

    CURL* curl = curl_easy_init();
    if (curl) {
        char url[256];
        // Determine receive_id_type based on prefix
        // oc_ = chat_id, ou_ = open_id, on_ = union_id, email = email
        const char* id_type = "open_id";
        if (strncmp(msg->chat_id.data, "oc_", 3) == 0) {
            id_type = "chat_id";
        } else if (strncmp(msg->chat_id.data, "ou_", 3) == 0) {
            id_type = "open_id";
        }
        
        snprintf(url, sizeof(url), "https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=%s", id_type);
        
        struct curl_slist* headers = NULL;
        char auth_header[256];
        if (data->access_token) {
            snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", data->access_token);
        } else {
            snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer invalid_token");
        }
        headers = curl_slist_append(headers, auth_header);
        headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");

        cJSON* json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "receive_id", msg->chat_id.data);

        // Determine message type
        bool is_image = false;
        bool is_audio = false;
        bool is_video = false;
        if (msg->attachments.count > 0) {
            char* filepath = msg->attachments.items[0].data;
            char* ext = strrchr(filepath, '.');
            if (ext) {
                if (strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) is_image = true;
                else if (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".wav") == 0 || strcasecmp(ext, ".opus") == 0) is_audio = true;
                else if (strcasecmp(ext, ".mp4") == 0) is_video = true;
            }
        }

        if (is_image) {
            char* key = upload_image(data, msg->attachments.items[0].data);
            if (key) {
                cJSON_AddStringToObject(json, "msg_type", "image");
                cJSON* contentObj = cJSON_CreateObject();
                cJSON_AddStringToObject(contentObj, "image_key", key);
                char* content_str = cJSON_PrintUnformatted(contentObj);
                cJSON_AddStringToObject(json, "content", content_str);
                free(content_str);
                cJSON_Delete(contentObj);
                free(key);
            }
        } else if (is_audio) {
            char* key = upload_file(data, msg->attachments.items[0].data, "stream");
            if (key) {
                cJSON_AddStringToObject(json, "msg_type", "audio");
                cJSON* contentObj = cJSON_CreateObject();
                cJSON_AddStringToObject(contentObj, "file_key", key);
                char* content_str = cJSON_PrintUnformatted(contentObj);
                cJSON_AddStringToObject(json, "content", content_str);
                free(content_str);
                cJSON_Delete(contentObj);
                free(key);
            }
        } else if (is_video) {
             char* key = upload_file(data, msg->attachments.items[0].data, "mp4");
             if (key) {
                cJSON_AddStringToObject(json, "msg_type", "media");
                cJSON* contentObj = cJSON_CreateObject();
                cJSON_AddStringToObject(contentObj, "file_key", key);
                char* content_str = cJSON_PrintUnformatted(contentObj);
                cJSON_AddStringToObject(json, "content", content_str);
                free(content_str);
                cJSON_Delete(contentObj);
                free(key);
             }
        } else {
            bool sent_card = false;
            if (data->config->use_card) {
                char* card_id = create_streaming_card(data, msg->content.data);
                if (card_id) {
                    cJSON_AddStringToObject(json, "msg_type", "interactive");
                    cJSON* contentObj = cJSON_CreateObject();
                    cJSON_AddStringToObject(contentObj, "type", "card");
                    cJSON* cardObj = cJSON_CreateObject();
                    cJSON_AddStringToObject(cardObj, "card_id", card_id);
                    cJSON_AddItemToObject(contentObj, "data", cardObj);
                    char* content_str = cJSON_PrintUnformatted(contentObj);
                    cJSON_AddStringToObject(json, "content", content_str);
                    free(content_str);
                    cJSON_Delete(contentObj);
                    free(card_id);
                    sent_card = true;
                }
            }

            if (!sent_card) {
                cJSON_AddStringToObject(json, "msg_type", "text");
                cJSON* contentObj = cJSON_CreateObject();
                cJSON_AddStringToObject(contentObj, "text", msg->content.data);
                char* content_str = cJSON_PrintUnformatted(contentObj);
                cJSON_AddStringToObject(json, "content", content_str);
                free(content_str);
                cJSON_Delete(contentObj);
            }
        }

        char* json_str = cJSON_PrintUnformatted(json);
        log_debug("[Feishu Debug] Sending Payload: %s", json_str);

        struct MemoryStruct chunk;
        chunk.memory = malloc(1);
        chunk.size = 0;

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            log_error("[Feishu] Send failed: %s", curl_easy_strerror(res));
        } else {
            log_debug("[Feishu Debug] Response: %s", chunk.memory);
            cJSON* resp = cJSON_Parse(chunk.memory);
            if (resp) {
                cJSON* code = cJSON_GetObjectItem(resp, "code");
                if (code && code->valueint != 0) {
                    cJSON* msg_item = cJSON_GetObjectItem(resp, "msg");
                    log_error("[Feishu] API Error: %d - %s", 
                            code->valueint, msg_item ? msg_item->valuestring : "Unknown");
                     if (code->valueint == 99991668 || code->valueint == 99991663) {
                        if (data->access_token) {
                            free(data->access_token);
                            data->access_token = NULL;
                        }
                    }
                } else {
                    log_info("[Feishu] Sent to %s", msg->chat_id.data);
                }
                cJSON_Delete(resp);
            }
        }

        free(chunk.memory);
        cJSON_Delete(json);
        free(json_str);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}

static void feishu_destroy(Channel* self) {
    FeishuChannelData* data = (FeishuChannelData*)self->user_data;
    if (data) {
        if (data->access_token) free(data->access_token);
        // if (data->ws) feishu_ws_destroy(data->ws);
        free(data);
    }
    free(self);
}

Channel* channel_create_feishu() {
    Channel* channel = malloc(sizeof(Channel));
    channel->name = strdup("feishu");
    channel->init = feishu_init;
    channel->start = feishu_start;
    channel->stop = feishu_stop;
    channel->send = feishu_send;
    channel->destroy = feishu_destroy;
    channel->user_data = NULL;
    return channel;
}
