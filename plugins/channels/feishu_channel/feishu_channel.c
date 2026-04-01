/*
 * Feishu Channel Plugin
 *
 * A plugin that demonstrates the Primagen channel plugin API.
 * This channel sends and receives messages via Feishu (Lark) bot.
 *
 * Build: make
 * Install: cp feishu_channel.so ../../.primagen/plugins/channels/
 */

#include "../../../src/include/channel.h"
#include "../../../src/include/common.h"
#include "../../../src/include/logger.h"
#include "../../../src/include/config.h"
#include "../../../src/plugin/plugin_manager.h"
#include "../../../src/bus/message_bus.h"
#include "../../../src/include/message.h"
#include "../../../src/vendor/cJSON/cJSON.h"
#include "../../../src/vendor/mongoose/mongoose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>

// =============================================================================
// Forward Declarations
// =============================================================================

typedef struct FeishuWS FeishuWS;

// Callback for received text messages
typedef void (*FeishuWSMessageCallback)(const char* chat_id, const char* content, const char* sender_id, void* user_data);

FeishuWS* feishu_ws_create(void);
void feishu_ws_destroy(FeishuWS* ws);
void feishu_ws_set_dns(FeishuWS* ws, const char* dns4, const char* dns6, int dns_timeout_ms, bool use_system_resolver);
bool feishu_ws_connect(FeishuWS* ws, const char* url);
void feishu_ws_run(FeishuWS* ws, FeishuWSMessageCallback callback, void* user_data);
void feishu_ws_stop(FeishuWS* ws);

// =============================================================================
// Channel Data
// =============================================================================

typedef struct {
    MessageBus* bus;
    bool running;
    char* access_token;
    pthread_t thread_id;
    FeishuWS* ws;
    // Plugin configuration
    char* app_id;
    char* app_secret;
    bool use_card;
    char* dns4;
    char* dns6;
    int dns_timeout_ms;
    bool use_system_resolver;
} FeishuChannelData;

struct MemoryStruct {
    char *memory;
    size_t size;
    bool done;
};

static bool memory_append_chunk(struct MemoryStruct* ms, const char* data, size_t len) {
    if (!ms) return false;
    if (len > 0 && !data) return false;
    if (len > SIZE_MAX - ms->size - 1) return false;
    size_t new_size = ms->size + len;
    char* new_mem = realloc(ms->memory, new_size + 1);
    if (!new_mem) return false;
    ms->memory = new_mem;
    if (len > 0) {
        memcpy(ms->memory + ms->size, data, len);
    }
    ms->size = new_size;
    ms->memory[ms->size] = '\0';
    return true;
}

static void apply_dns_config(struct mg_mgr* mgr, const FeishuChannelData* data) {
    if (!mgr || !data) return;
    mgr->use_system_resolver = data->use_system_resolver;
    if (data->use_system_resolver) return;
    if (data->dns4 && data->dns4[0]) mgr->dns4.url = data->dns4;
    if (data->dns6 && data->dns6[0]) mgr->dns6.url = data->dns6;
    if (data->dns_timeout_ms > 0) mgr->dnstimeout = data->dns_timeout_ms;
}

// =============================================================================
// Internal Helper Functions
// =============================================================================

static void fn(struct mg_connection *c, int ev, void *ev_data) {
    struct MemoryStruct *ms = (struct MemoryStruct *) c->fn_data;
    if (!ms) return;
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        if (!memory_append_chunk(ms, hm->body.buf, hm->body.len)) {
            ms->done = true;
            c->is_closing = 1;
            return;
        }
        c->is_closing = 1;
        ms->done = true;
    } else if (ev == MG_EV_ERROR) {
        ms->done = true;
    }
}

static void refresh_token(FeishuChannelData* data) {
    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';

    mg_mgr_init(&mgr);
    apply_dns_config(&mgr, data);

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "app_id", data->app_id);
    cJSON_AddStringToObject(payload, "app_secret", data->app_secret);
    char* json_str = cJSON_PrintUnformatted(payload);

    const char* url = "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal";

    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        log_error("[Feishu] Failed to connect for token refresh");
        free(json_str);
        cJSON_Delete(payload);
        mg_mgr_free(&mgr);
        free(chunk.memory);
        return;
    }

    struct mg_str host = mg_url_host(url);

    struct mg_tls_opts opts = {0};
    opts.ca = mg_str("");
    opts.name = host;
    opts.skip_verification = true;
    if (mg_url_is_ssl(url)) {
        mg_tls_init(c, &opts);
    }

    mg_printf(c,
        "POST %s HTTP/1.0\r\n"
        "Host: %.*s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
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
                }
            }
            cJSON_Delete(resp);
        } else {
            log_error("[Feishu] Failed to parse token response. Raw body: '%s'", chunk.memory);
        }
    } else {
        log_error("[Feishu] Empty response for token refresh");
    }

    free(chunk.memory);
    free(json_str);
    cJSON_Delete(payload);
    mg_mgr_free(&mgr);
}

static char* get_ws_url(FeishuChannelData* data) {
    if (!data->access_token) refresh_token(data);
    char* ws_url = NULL;

    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';

    mg_mgr_init(&mgr);
    apply_dns_config(&mgr, data);

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "AppID", data->app_id);
    cJSON_AddStringToObject(payload, "AppSecret", data->app_secret);
    char* json_str = cJSON_PrintUnformatted(payload);

    const char* url = "https://open.feishu.cn/callback/ws/endpoint";

    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        log_error("[Feishu] Failed to connect for WS URL");
        free(json_str);
        cJSON_Delete(payload);
        mg_mgr_free(&mgr);
        free(chunk.memory);
        return NULL;
    }

    struct mg_str host = mg_url_host(url);

    struct mg_tls_opts opts = {0};
    opts.ca = mg_str("");
    opts.name = host;
    opts.skip_verification = true;
    if (mg_url_is_ssl(url)) {
        mg_tls_init(c, &opts);
    }

    mg_printf(c,
        "POST %s HTTP/1.0\r\n"
        "Host: %.*s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "locale: zh\r\n"
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
            cJSON* code = cJSON_GetObjectItem(resp, "code");
            if (code && code->valueint == 0) {
                cJSON* dataObj = cJSON_GetObjectItem(resp, "data");
                if (dataObj) {
                    cJSON* urlItem = cJSON_GetObjectItem(dataObj, "URL");
                    if (cJSON_IsString(urlItem)) {
                        ws_url = strdup(urlItem->valuestring);
                    }
                }
            } else {
                cJSON* msg = cJSON_GetObjectItem(resp, "msg");
                log_error("[Feishu] Get WS URL failed: %d - %s",
                        code ? code->valueint : -1, msg ? msg->valuestring : "Unknown");
            }
            cJSON_Delete(resp);
        } else {
            log_error("[Feishu] Failed to parse WS URL response. Raw body: '%s'", chunk.memory);
        }
    }

    free(chunk.memory);
    free(json_str);
    cJSON_Delete(payload);
    mg_mgr_free(&mgr);

    return ws_url;
}

static char* create_streaming_card(FeishuChannelData* data, const char* content) {
    if (!data->access_token) refresh_token(data);
    char* card_id = NULL;

    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';

    mg_mgr_init(&mgr);
    apply_dns_config(&mgr, data);

    // Construct Card JSON
    cJSON* cardData = cJSON_CreateObject();
    cJSON_AddStringToObject(cardData, "schema", "2.0");

    cJSON* header = cJSON_CreateObject();
    cJSON_AddStringToObject(header, "template", "blue");
    cJSON* title = cJSON_CreateObject();
    cJSON_AddStringToObject(title, "tag", "plain_text");
    cJSON_AddStringToObject(title, "content", "Primagen");
    cJSON_AddItemToObject(header, "title", title);
    cJSON_AddItemToObject(cardData, "header", header);

    cJSON* config = cJSON_CreateObject();
    cJSON_AddBoolToObject(config, "streaming_mode", true);
    cJSON_AddItemToObject(cardData, "config", config);

    cJSON* body = cJSON_CreateObject();
    cJSON* elements = cJSON_CreateArray();
    cJSON* element = cJSON_CreateObject();
    cJSON_AddStringToObject(element, "tag", "markdown");
    cJSON_AddStringToObject(element, "element_id", "markdown_1");
    cJSON_AddStringToObject(element, "content", content);
    cJSON_AddItemToArray(elements, element);
    cJSON_AddItemToObject(body, "elements", elements);
    cJSON_AddItemToObject(cardData, "body", body);

    char* card_data_str = cJSON_PrintUnformatted(cardData);

    cJSON* reqBody = cJSON_CreateObject();
    cJSON_AddStringToObject(reqBody, "type", "card_json");
    cJSON_AddStringToObject(reqBody, "data", card_data_str);
    char* req_json_str = cJSON_PrintUnformatted(reqBody);

    const char* url = "https://open.feishu.cn/open-apis/cardkit/v1/cards";

    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        log_error("[Feishu] Failed to connect for Card creation");
        free(req_json_str);
        free(card_data_str);
        cJSON_Delete(reqBody);
        cJSON_Delete(cardData);
        mg_mgr_free(&mgr);
        free(chunk.memory);
        return NULL;
    }

    struct mg_str host = mg_url_host(url);

    struct mg_tls_opts opts = {0};
    opts.ca = mg_str("");
    opts.name = host;
    opts.skip_verification = true;
    if (mg_url_is_ssl(url)) {
        mg_tls_init(c, &opts);
    }

    mg_printf(c,
        "POST %s HTTP/1.0\r\n"
        "Host: %.*s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        mg_url_uri(url),
        (int)host.len, host.buf,
        data->access_token ? data->access_token : "",
        (int)strlen(req_json_str),
        req_json_str
    );

    while (!chunk.done) mg_mgr_poll(&mgr, 1000);

    if (chunk.size > 0) {
        cJSON* resp = cJSON_Parse(chunk.memory);
        if (resp) {
            cJSON* code = cJSON_GetObjectItem(resp, "code");
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
            cJSON_Delete(resp);
        }
    }

    free(chunk.memory);
    free(card_data_str);
    free(req_json_str);
    cJSON_Delete(cardData);
    cJSON_Delete(reqBody);
    mg_mgr_free(&mgr);

    return card_id;
}

typedef struct {
    char type[16];
    char path[1024];
    char cover_path[1024];
    int duration;
} FeishuAttachment;

static bool append_literal(struct MemoryStruct* body, const char* text) {
    return memory_append_chunk(body, text, strlen(text));
}

static const char* file_basename(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool read_file_bytes(const char* path, unsigned char** out_bytes, size_t* out_len) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) return false;
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    unsigned char* bytes = malloc((size_t)st.st_size);
    if (!bytes) {
        fclose(fp);
        return false;
    }
    size_t n = fread(bytes, 1, (size_t)st.st_size, fp);
    fclose(fp);
    if (n != (size_t)st.st_size) {
        free(bytes);
        return false;
    }
    *out_bytes = bytes;
    *out_len = n;
    return true;
}

static const char* infer_media_type_from_path(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot || *(dot + 1) == '\0') return NULL;
    const char* ext = dot + 1;
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0 ||
        strcasecmp(ext, "png") == 0 || strcasecmp(ext, "webp") == 0 ||
        strcasecmp(ext, "gif") == 0 || strcasecmp(ext, "bmp") == 0 ||
        strcasecmp(ext, "ico") == 0 || strcasecmp(ext, "tiff") == 0 ||
        strcasecmp(ext, "heic") == 0) return "image";
    if (strcasecmp(ext, "opus") == 0 || strcasecmp(ext, "mp3") == 0 ||
        strcasecmp(ext, "wav") == 0 || strcasecmp(ext, "m4a") == 0 ||
        strcasecmp(ext, "aac") == 0 || strcasecmp(ext, "ogg") == 0 ||
        strcasecmp(ext, "amr") == 0) return "audio";
    if (strcasecmp(ext, "mp4") == 0 || strcasecmp(ext, "mov") == 0 ||
        strcasecmp(ext, "m4v") == 0 || strcasecmp(ext, "avi") == 0 ||
        strcasecmp(ext, "mkv") == 0 || strcasecmp(ext, "webm") == 0) return "video";
    return NULL;
}

static bool parse_attachment_spec(const char* raw, FeishuAttachment* out) {
    if (!raw || !out) return false;
    memset(out, 0, sizeof(*out));
    out->duration = 0;

    if (raw[0] == '{') {
        cJSON* json = cJSON_Parse(raw);
        if (!json) return false;
        cJSON* type = cJSON_GetObjectItem(json, "type");
        cJSON* path = cJSON_GetObjectItem(json, "path");
        cJSON* duration = cJSON_GetObjectItem(json, "duration");
        cJSON* cover_path = cJSON_GetObjectItem(json, "cover_path");
        if (!cJSON_IsString(type) || !type->valuestring || !cJSON_IsString(path) || !path->valuestring) {
            cJSON_Delete(json);
            return false;
        }
        snprintf(out->type, sizeof(out->type), "%s", type->valuestring);
        snprintf(out->path, sizeof(out->path), "%s", path->valuestring);
        if (cJSON_IsNumber(duration) && duration->valueint > 0) {
            out->duration = duration->valueint;
        }
        if (cJSON_IsString(cover_path) && cover_path->valuestring) {
            snprintf(out->cover_path, sizeof(out->cover_path), "%s", cover_path->valuestring);
        }
        cJSON_Delete(json);
        return true;
    }

    const char* inferred = infer_media_type_from_path(raw);
    if (!inferred) return false;
    snprintf(out->type, sizeof(out->type), "%s", inferred);
    snprintf(out->path, sizeof(out->path), "%s", raw);
    return true;
}

static bool build_feishu_upload_body(const char* boundary, const char* file_field, const char* file_path,
                                     const char* primary_field_name, const char* primary_field_value,
                                     const char* secondary_field_name, const char* secondary_field_value,
                                     struct MemoryStruct* body) {
    unsigned char* file_bytes = NULL;
    size_t file_len = 0;
    if (!read_file_bytes(file_path, &file_bytes, &file_len)) return false;

    char line[2048];
    if (snprintf(line, sizeof(line), "--%s\r\n", boundary) < 0 || !append_literal(body, line)) goto fail;
    if (snprintf(line, sizeof(line),
                 "Content-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n",
                 primary_field_name, primary_field_value) < 0 || !append_literal(body, line)) goto fail;

    if (secondary_field_name && secondary_field_value) {
        if (snprintf(line, sizeof(line), "--%s\r\n", boundary) < 0 || !append_literal(body, line)) goto fail;
        if (snprintf(line, sizeof(line),
                     "Content-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n",
                     secondary_field_name, secondary_field_value) < 0 || !append_literal(body, line)) goto fail;
    }

    if (snprintf(line, sizeof(line), "--%s\r\n", boundary) < 0 || !append_literal(body, line)) goto fail;
    if (snprintf(line, sizeof(line),
                 "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
                 "Content-Type: application/octet-stream\r\n\r\n",
                 file_field, file_basename(file_path)) < 0 || !append_literal(body, line)) goto fail;

    if (!memory_append_chunk(body, (const char*)file_bytes, file_len)) goto fail;
    if (!append_literal(body, "\r\n")) goto fail;
    if (snprintf(line, sizeof(line), "--%s--\r\n", boundary) < 0 || !append_literal(body, line)) goto fail;

    free(file_bytes);
    return true;
fail:
    free(file_bytes);
    return false;
}

static bool feishu_post_json_with_retry(FeishuChannelData* data, const char* url, const char* json_str) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!data->access_token) refresh_token(data);
        if (!data->access_token) {
            log_error("[Feishu] Not connected (no token)");
            return false;
        }

        struct mg_mgr mgr;
        struct MemoryStruct chunk = {0};
        chunk.memory = malloc(1);
        chunk.memory[0] = '\0';
        mg_mgr_init(&mgr);
        apply_dns_config(&mgr, data);

        struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
        if (!c) {
            log_error("[Feishu] Failed to connect");
            mg_mgr_free(&mgr);
            free(chunk.memory);
            return false;
        }

        struct mg_str host = mg_url_host(url);
        struct mg_tls_opts opts = {0};
        opts.ca = mg_str("");
        opts.name = host;
        opts.skip_verification = true;
        if (mg_url_is_ssl(url)) mg_tls_init(c, &opts);

        mg_printf(c,
            "POST %s HTTP/1.0\r\n"
            "Host: %.*s\r\n"
            "Content-Type: application/json; charset=utf-8\r\n"
            "Authorization: Bearer %s\r\n"
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

        bool retry = false;
        bool success = false;
        if (chunk.size > 0) {
            cJSON* resp = cJSON_Parse(chunk.memory);
            if (resp) {
                cJSON* code = cJSON_GetObjectItem(resp, "code");
                if (code && code->valueint != 0) {
                    cJSON* msg_item = cJSON_GetObjectItem(resp, "msg");
                    log_error("[Feishu] API Error: %d - %s",
                              code->valueint, msg_item ? msg_item->valuestring : "Unknown");
                    if ((code->valueint == 99991668 || code->valueint == 99991663) && attempt == 0) {
                        if (data->access_token) {
                            free(data->access_token);
                            data->access_token = NULL;
                        }
                        refresh_token(data);
                        retry = true;
                        log_warn("[Feishu] Access token refreshed, retry sending once");
                    }
                } else {
                    success = true;
                }
                cJSON_Delete(resp);
            }
        } else {
            log_error("[Feishu] Request failed: Empty response");
        }

        mg_mgr_free(&mgr);
        free(chunk.memory);
        if (success) return true;
        if (!retry) break;
    }
    return false;
}

static bool feishu_upload_media(FeishuChannelData* data, const FeishuAttachment* attachment,
                                char** out_file_key, char** out_image_key) {
    const char* url = NULL;
    const char* upload_file_type = NULL;
    bool is_image = strcmp(attachment->type, "image") == 0;

    if (is_image) {
        url = "https://open.feishu.cn/open-apis/im/v1/images";
    } else {
        url = "https://open.feishu.cn/open-apis/im/v1/files";
        upload_file_type = strcmp(attachment->type, "audio") == 0 ? "opus" : "mp4";
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        if (!data->access_token) refresh_token(data);
        if (!data->access_token) return false;

        const char* boundary = "----PrimagenFeishuBoundary";
        struct MemoryStruct body = {0};
        body.memory = malloc(1);
        body.memory[0] = '\0';

        bool built = false;
        if (is_image) {
            built = build_feishu_upload_body(boundary, "image", attachment->path,
                                             "image_type", "message",
                                             NULL, NULL, &body);
        } else {
            built = build_feishu_upload_body(boundary, "file", attachment->path,
                                             "file_type", upload_file_type,
                                             "file_name", file_basename(attachment->path), &body);
        }
        if (!built) {
            free(body.memory);
            log_error("[Feishu] Failed to build upload body: %s", attachment->path);
            return false;
        }

        struct mg_mgr mgr;
        struct MemoryStruct chunk = {0};
        chunk.memory = malloc(1);
        chunk.memory[0] = '\0';
        mg_mgr_init(&mgr);
        apply_dns_config(&mgr, data);

        struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
        if (!c) {
            mg_mgr_free(&mgr);
            free(body.memory);
            free(chunk.memory);
            log_error("[Feishu] Failed to connect for media upload");
            return false;
        }

        struct mg_str host = mg_url_host(url);
        struct mg_tls_opts opts = {0};
        opts.ca = mg_str("");
        opts.name = host;
        opts.skip_verification = true;
        if (mg_url_is_ssl(url)) mg_tls_init(c, &opts);

        mg_printf(c,
            "POST %s HTTP/1.0\r\n"
            "Host: %.*s\r\n"
            "Authorization: Bearer %s\r\n"
            "Content-Type: multipart/form-data; boundary=%s\r\n"
            "Content-Length: %d\r\n"
            "\r\n",
            mg_url_uri(url),
            (int)host.len, host.buf,
            data->access_token,
            boundary,
            (int)body.size
        );
        mg_send(c, body.memory, body.size);

        while (!chunk.done) mg_mgr_poll(&mgr, 1000);

        bool retry = false;
        bool ok = false;
        if (chunk.size > 0) {
            cJSON* resp = cJSON_Parse(chunk.memory);
            if (resp) {
                cJSON* code = cJSON_GetObjectItem(resp, "code");
                if (code && code->valueint == 0) {
                    cJSON* data_obj = cJSON_GetObjectItem(resp, "data");
                    if (data_obj) {
                        if (is_image) {
                            cJSON* image_key = cJSON_GetObjectItem(data_obj, "image_key");
                            if (cJSON_IsString(image_key) && image_key->valuestring) {
                                *out_image_key = strdup(image_key->valuestring);
                                ok = *out_image_key != NULL;
                            }
                        } else {
                            cJSON* file_key = cJSON_GetObjectItem(data_obj, "file_key");
                            if (cJSON_IsString(file_key) && file_key->valuestring) {
                                *out_file_key = strdup(file_key->valuestring);
                                ok = *out_file_key != NULL;
                            }
                        }
                    }
                } else {
                    cJSON* msg = cJSON_GetObjectItem(resp, "msg");
                    log_error("[Feishu] Upload failed: %d - %s",
                              code ? code->valueint : -1, msg ? msg->valuestring : "Unknown");
                    if ((code->valueint == 99991668 || code->valueint == 99991663) && attempt == 0) {
                        if (data->access_token) {
                            free(data->access_token);
                            data->access_token = NULL;
                        }
                        refresh_token(data);
                        retry = true;
                        log_warn("[Feishu] Access token refreshed, retry upload once");
                    }
                }
                cJSON_Delete(resp);
            }
        }

        mg_mgr_free(&mgr);
        free(body.memory);
        free(chunk.memory);
        if (ok) return true;
        if (!retry) break;
    }
    return false;
}

static bool feishu_send_attachment(FeishuChannelData* data, const char* receive_id_type, const char* receive_id,
                                   const FeishuAttachment* attachment) {
    char url[512];
    snprintf(url, sizeof(url), "https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=%s", receive_id_type);

    char* file_key = NULL;
    char* image_key = NULL;
    if (!feishu_upload_media(data, attachment, &file_key, &image_key)) {
        free(file_key);
        free(image_key);
        return false;
    }

    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "receive_id", receive_id);

    if (strcmp(attachment->type, "image") == 0) {
        cJSON_AddStringToObject(json, "msg_type", "image");
        cJSON* content_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(content_obj, "image_key", image_key);
        char* content_str = cJSON_PrintUnformatted(content_obj);
        cJSON_AddStringToObject(json, "content", content_str ? content_str : "{}");
        free(content_str);
        cJSON_Delete(content_obj);
    } else if (strcmp(attachment->type, "audio") == 0) {
        cJSON_AddStringToObject(json, "msg_type", "audio");
        cJSON* content_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(content_obj, "file_key", file_key);
        if (attachment->duration > 0) {
            cJSON_AddNumberToObject(content_obj, "duration", attachment->duration);
        }
        char* content_str = cJSON_PrintUnformatted(content_obj);
        cJSON_AddStringToObject(json, "content", content_str ? content_str : "{}");
        free(content_str);
        cJSON_Delete(content_obj);
    } else {
        cJSON_AddStringToObject(json, "msg_type", "file");
        cJSON* content_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(content_obj, "file_key", file_key);
        char* content_str = cJSON_PrintUnformatted(content_obj);
        cJSON_AddStringToObject(json, "content", content_str ? content_str : "{}");
        free(content_str);
        cJSON_Delete(content_obj);
    }

    char* json_str = cJSON_PrintUnformatted(json);
    bool ok = json_str && feishu_post_json_with_retry(data, url, json_str);
    if (!ok) {
        log_error("[Feishu] Failed to send attachment message: %s", attachment->path);
    }

    free(json_str);
    cJSON_Delete(json);
    free(file_key);
    free(image_key);
    return ok;
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
            feishu_ws_set_dns(data->ws, data->dns4, data->dns6, data->dns_timeout_ms, data->use_system_resolver);
            if (feishu_ws_connect(data->ws, url)) {
                log_info("[Feishu] WebSocket connected.");
                feishu_ws_run(data->ws, on_feishu_message, data);
            } else {
                log_error("[Feishu] WebSocket connect failed.");
            }
            feishu_ws_destroy(data->ws);
            data->ws = NULL;
            free(url);
        } else {
            log_error("[Feishu] Failed to get WebSocket URL");
        }

        if (data->running) sleep(5);
    }
    return NULL;
}

// =============================================================================
// Channel Implementation
// =============================================================================

static bool feishu_init(Channel* self, Config* cfg, MessageBus* bus) {
    FeishuChannelData* data = malloc(sizeof(FeishuChannelData));
    if (!data) return false;

    data->bus = bus;
    data->running = false;
    data->access_token = NULL;
    data->ws = NULL;
    data->dns4 = NULL;
    data->dns6 = NULL;
    data->dns_timeout_ms = 0;
    data->use_system_resolver = false;

    // Get plugin configuration
    PluginConfig* plugin_cfg = config_get_plugin_config(cfg, "feishu_channel");
    if (plugin_cfg && plugin_cfg->config) {
        cJSON* app_id = cJSON_GetObjectItem(plugin_cfg->config, "app_id");
        cJSON* app_secret = cJSON_GetObjectItem(plugin_cfg->config, "app_secret");
        cJSON* use_card = cJSON_GetObjectItem(plugin_cfg->config, "use_card");

        data->app_id = app_id && cJSON_IsString(app_id) ? strdup(app_id->valuestring) : strdup("");
        data->app_secret = app_secret && cJSON_IsString(app_secret) ? strdup(app_secret->valuestring) : strdup("");
        data->use_card = use_card && cJSON_IsBool(use_card) ? use_card->valueint : false;
    } else {
        data->app_id = strdup("");
        data->app_secret = strdup("");
        data->use_card = false;
    }

    DNSConfig* dns_cfg = config_get_dns_config(cfg);
    if (dns_cfg) {
        if (dns_cfg->dns4 && dns_cfg->dns4[0]) data->dns4 = strdup(dns_cfg->dns4);
        if (dns_cfg->dns6 && dns_cfg->dns6[0]) data->dns6 = strdup(dns_cfg->dns6);
        if (dns_cfg->dns_timeout_ms > 0) data->dns_timeout_ms = dns_cfg->dns_timeout_ms;
        data->use_system_resolver = dns_cfg->use_system_resolver;
    }

    self->user_data = data;
    log_info("[Feishu] Initialized with app_id: %s", data->app_id);
    return true;
}

static void feishu_start(Channel* self) {
    FeishuChannelData* data = (FeishuChannelData*)self->user_data;
    // Check if app_id is configured (plugin is enabled if app_id is set)
    if (!data->app_id || !data->app_id[0]) {
        log_info("[Feishu] Channel not started (no app_id configured)");
        return;
    }

    data->running = true;
    refresh_token(data);

    // Start receive thread
    pthread_create(&data->thread_id, NULL, feishu_receive_loop, data);
}

static void feishu_stop(Channel* self) {
    FeishuChannelData* data = (FeishuChannelData*)self->user_data;
    data->running = false;
    if (data->ws) feishu_ws_stop(data->ws);
}

static void feishu_send(Channel* self, OutboundMessage* msg) {
    FeishuChannelData* data = (FeishuChannelData*)self->user_data;
    // Check if channel is configured
    if (!data->app_id || !data->app_id[0]) return;

    if (strcmp(msg->channel.data, "feishu") != 0) return;
    log_info("[Feishu] Reply content: %s", msg->content.data);

    if (!data->access_token) refresh_token(data);

    if (!data->access_token) {
        log_error("[Feishu] Not connected (no token)");
        return;
    }

    char url[512];
    const char* id_type = "open_id";
    if (strncmp(msg->chat_id.data, "oc_", 3) == 0) {
        id_type = "chat_id";
    } else if (strncmp(msg->chat_id.data, "ou_", 3) == 0) {
        id_type = "open_id";
    }

    snprintf(url, sizeof(url), "https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=%s", id_type);

    bool has_text = msg->content.data && msg->content.data[0] != '\0';
    log_debug("[Feishu] attachments.count=%zu", msg->attachments.count);
    if (msg->attachments.count > 0) {
        for (size_t i = 0; i < msg->attachments.count; i++) {
            FeishuAttachment attachment;
            log_debug("[Feishu] Processing attachment[%zu]: %s", i, msg->attachments.items[i].data);
            if (!parse_attachment_spec(msg->attachments.items[i].data, &attachment)) {
                log_error("[Feishu] Invalid attachment spec: %s", msg->attachments.items[i].data);
                continue;
            }
            log_debug("[Feishu] Parsed attachment: type=%s, path=%s", attachment.type, attachment.path);
            if (!feishu_send_attachment(data, id_type, msg->chat_id.data, &attachment)) {
                log_error("[Feishu] Attachment delivery failed: %s", attachment.path);
            }
        }
        if (!has_text) return;
    }

    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "receive_id", msg->chat_id.data);

    bool sent_card = false;
    if (data->use_card) {
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
        // Use post message type with rich text format
        // Note: zh_cn needs title and content (2D array for paragraphs)
        cJSON_AddStringToObject(json, "msg_type", "post");
        cJSON* contentObj = cJSON_CreateObject();
        cJSON* zhCN = cJSON_CreateObject();

        // Add required title field
        cJSON_AddStringToObject(zhCN, "title", "");

        // Content is a 2D array - each inner array is a paragraph
        cJSON* content = cJSON_CreateArray();
        cJSON* paragraph = cJSON_CreateArray();  // Each paragraph is an array of elements
        cJSON* textItem = cJSON_CreateObject();
        cJSON_AddStringToObject(textItem, "tag", "text");
        cJSON_AddStringToObject(textItem, "text", msg->content.data);
        cJSON_AddItemToArray(paragraph, textItem);
        cJSON_AddItemToArray(content, paragraph);
        cJSON_AddItemToObject(zhCN, "content", content);

        cJSON_AddItemToObject(contentObj, "post", zhCN);
        char* content_str = cJSON_PrintUnformatted(contentObj);
        cJSON_AddStringToObject(json, "content", content_str);
        free(content_str);
        cJSON_Delete(contentObj);
    }

    char* json_str = cJSON_PrintUnformatted(json);
    log_debug("[Feishu Debug] Sending Payload: %s", json_str ? json_str : "");
    if (!json_str || !feishu_post_json_with_retry(data, url, json_str)) {
        log_error("[Feishu] Failed to send text/card message");
    }

    cJSON_Delete(json);
    free(json_str);
}

static void feishu_destroy(Channel* self) {
    if (self->user_data) {
        FeishuChannelData* data = (FeishuChannelData*)self->user_data;
        if (data->access_token) free(data->access_token);
        if (data->app_id) free(data->app_id);
        if (data->app_secret) free(data->app_secret);
        if (data->dns4) free(data->dns4);
        if (data->dns6) free(data->dns6);
        if (data->ws) feishu_ws_destroy(data->ws);
        free(data);
    }
    free(self);
}

// =============================================================================
// Channel Factory
// =============================================================================

static Channel* feishu_channel_create(void) {
    Channel* channel = calloc(1, sizeof(Channel));
    if (!channel) return NULL;

    channel->name = strdup("feishu");
    channel->init = feishu_init;
    channel->start = feishu_start;
    channel->stop = feishu_stop;
    channel->send = feishu_send;
    channel->destroy = feishu_destroy;
    channel->user_data = NULL;
    channel->plugin_ref = NULL;

    log_info("[FeishuChannel] Created new channel instance");
    return channel;
}

// =============================================================================
// Plugin Initialization
// =============================================================================

PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context) {
    (void)context;

    log_info("[Plugin:feishu_channel] Initializing feishu channel plugin");

    // Register the channel factory
    int ret = plugin_register_channel(manager, NULL, "feishu", feishu_channel_create);

    if (ret == 0) {
        log_info("[Plugin:feishu_channel] Successfully registered feishu channel");
    } else {
        log_error("[Plugin:feishu_channel] Failed to register feishu channel");
        return -1;
    }

    return 0;
}

PLUGIN_EXPORT int plugin_cleanup(void) {
    log_info("[Plugin:feishu_channel] Cleaning up feishu channel plugin");
    return 0;
}

// =============================================================================
// Plugin Information
// =============================================================================

static PluginInfo g_plugin_info = {
    .version = 1,
    .type = PLUGIN_CHANNEL,
    .name = "feishu_channel",
    .description = "Feishu (Lark) bot channel for Primagen",
    .plugin_id = "feishu_channel"
};

PLUGIN_EXPORT PluginInfo* plugin_get_info(void) {
    return &g_plugin_info;
}
