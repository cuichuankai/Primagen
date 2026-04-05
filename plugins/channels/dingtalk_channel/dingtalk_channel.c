/*
 * DingTalk Channel Plugin
 *
 * Provides DingTalk channel support for Primagen.
 * Uses DingTalk APIs for token refresh, WebSocket receive, and message send.
 *
 * Configuration (.primagen/config.json):
 *   "plugins": [{
 *     "plugin_id": "dingtalk_channel",
 *     "enabled": true,
 *     "config": {
 *       "clientId": "your_robot_client_id",
 *       "clientSecret": "your_robot_client_secret"
 *     }
 *   }]
 *
 * Runtime behavior:
 *   - Receives messages via DingTalk WebSocket.
 *   - Replies through session webhook when available.
 *   - Sends attachments through media/upload + chat/send.
 *   - If image attachment upload fails and "<file>.url" exists,
 *     falls back to markdown image link delivery.
 *
 * Build: make
 * Install: make install
 */

#include "../../../src/include/channel.h"
#include "../../../src/include/config.h"
#include "../../../src/include/message.h"
#include "../../../src/bus/message_bus.h"
#include "../../../src/vendor/cJSON/cJSON.h"
#include "../../../src/vendor/mongoose/mongoose.h"
#include "../../../src/include/logger.h"
#include "../../../src/plugin/plugin_manager.h"
#include "dingtalk_ws.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>

// =============================================================================
// Channel Data
// =============================================================================

// Session webhook storage for conversation replies
typedef struct {
    char* conversation_id;
    char* session_webhook;
    time_t expiry;
} SessionWebhook;

typedef struct {
    MessageBus* bus;
    bool running;
    char* access_token;
    time_t token_expiry;
    pthread_t thread_id;
    DingTalkWS* ws;
    // Plugin configuration
    char* client_id;
    char* client_secret;
    char* dns4;
    char* dns6;
    int dns_timeout_ms;
    bool use_system_resolver;
    // Session webhook cache (for replying to conversations)
    SessionWebhook* session_cache;
    size_t session_cache_size;
} DingTalkChannelData;

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

static void apply_dns_config(struct mg_mgr* mgr, const DingTalkChannelData* data) {
    if (!mgr || !data) return;
    mgr->use_system_resolver = data->use_system_resolver;
    if (data->use_system_resolver) return;
    if (data->dns4 && data->dns4[0]) mgr->dns4.url = data->dns4;
    if (data->dns6 && data->dns6[0]) mgr->dns6.url = data->dns6;
    if (data->dns_timeout_ms > 0) mgr->dnstimeout = data->dns_timeout_ms;
}

// =============================================================================
// HTTP Callback for Mongoose
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

// Separate callback for webhook sends that handles responses without Content-Length
// DingTalk session webhook responses don't include Content-Length, so we accumulate
// data as it arrives via MG_EV_READ and mark done on MG_EV_CLOSE
static void webhook_fn(struct mg_connection *c, int ev, void *ev_data) {
    struct MemoryStruct *ms = (struct MemoryStruct *) c->fn_data;
    if (!ms) return;

    if (ev == MG_EV_HTTP_MSG) {
        // Normal case: response has Content-Length or is chunked
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        if (hm->body.len > 0) {
            if (!memory_append_chunk(ms, hm->body.buf, hm->body.len)) {
                ms->done = true;
                c->is_closing = 1;
                return;
            }
            c->is_closing = 1;
            ms->done = true;
            log_debug("[DingTalk] Webhook response received via MG_EV_HTTP_MSG (%zu bytes)", hm->body.len);
        } else {
            // Empty body in HTTP_MSG - response may come via MG_EV_READ (no Content-Length)
            // Don't set done=true yet, wait for MG_EV_CLOSE
            log_debug("[DingTalk] Webhook HTTP_MSG with empty body, waiting for data...");
        }
    } else if (ev == MG_EV_READ) {
        // Data received incrementally (for responses without Content-Length)
        // ev_data is a long* pointing to number of bytes read
        long bytes_read = *(long *) ev_data;
        if (bytes_read > 0) {
            // Get the data that was just read from c->recv
            // The new data is at the end of c->recv buffer
            if (c->recv.len >= (size_t) bytes_read) {
                const char *new_data = (char *) c->recv.buf + (c->recv.len - bytes_read);
                if (!memory_append_chunk(ms, new_data, (size_t)bytes_read)) {
                    ms->done = true;
                    c->is_closing = 1;
                    return;
                }
                log_debug("[DingTalk] Webhook data received via MG_EV_READ (%ld bytes)", bytes_read);
            }
        }
    } else if (ev == MG_EV_CLOSE) {
        // Connection closed - this is expected for responses without Content-Length
        // Data has already been received via MG_EV_READ events
        log_debug("[DingTalk] Webhook connection closed (total: %zu bytes)", ms->size);
        ms->done = true;
    } else if (ev == MG_EV_ERROR) {
        log_error("[DingTalk] Webhook connection error");
        ms->done = true;
    }
}

// =============================================================================
// Helper Functions
// =============================================================================

static void refresh_token(DingTalkChannelData* data) {
    if (!data->client_id || !data->client_secret) {
        log_error("[DingTalk] Missing client_id or client_secret");
        return;
    }

    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';

    mg_mgr_init(&mgr);

    apply_dns_config(&mgr, data);

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "appKey", data->client_id);
    cJSON_AddStringToObject(payload, "appSecret", data->client_secret);
    char* json_str = cJSON_PrintUnformatted(payload);

    const char* url = "https://api.dingtalk.com/v1.0/oauth2/accessToken";

    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        log_error("[DingTalk] Failed to connect for token refresh");
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
                data->token_expiry = time(NULL) + 7200; // 2 hours
                log_info("[DingTalk] Token refreshed successfully");
            } else {
                log_error("[DingTalk] Failed to get access token");
            }
            cJSON_Delete(resp);
        }
    } else {
        log_error("[DingTalk] Empty response from token endpoint");
    }

    free(chunk.memory);
    free(json_str);
    cJSON_Delete(payload);
    mg_mgr_free(&mgr);
}

static bool is_token_valid(DingTalkChannelData* data) {
    return data->access_token != NULL && time(NULL) < data->token_expiry - 300;
}

typedef struct {
    char type[16];
    char path[1024];
    char url[2048];
    int duration;
} DingTalkAttachment;

static bool append_text_chunk(struct MemoryStruct* ms, const char* text) {
    return memory_append_chunk(ms, text, strlen(text));
}

static const char* dingtalk_file_basename(const char* path) {
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

static bool read_first_line(const char* path, char** out_line) {
    if (!path || !out_line) return false;
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    char buf[4096];
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return false;
    }
    fclose(fp);
    size_t len = strcspn(buf, "\r\n");
    buf[len] = '\0';
    if (len == 0) return false;
    *out_line = strdup(buf);
    return *out_line != NULL;
}

static bool append_markdown_line(char** dest, const char* line) {
    if (!dest || !line || line[0] == '\0') return false;
    if (!*dest) {
        *dest = strdup(line);
        return *dest != NULL;
    }
    size_t old_len = strlen(*dest);
    size_t line_len = strlen(line);
    if (old_len > SIZE_MAX - line_len - 3) return false;
    char* next = realloc(*dest, old_len + line_len + 3);
    if (!next) return false;
    next[old_len] = '\n';
    next[old_len + 1] = '\n';
    memcpy(next + old_len + 2, line, line_len + 1);
    *dest = next;
    return true;
}

static const char* infer_attachment_type(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot || *(dot + 1) == '\0') return NULL;
    const char* ext = dot + 1;
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0 ||
        strcasecmp(ext, "png") == 0 || strcasecmp(ext, "gif") == 0 ||
        strcasecmp(ext, "webp") == 0 || strcasecmp(ext, "bmp") == 0) return "image";
    if (strcasecmp(ext, "mp3") == 0 || strcasecmp(ext, "wav") == 0 ||
        strcasecmp(ext, "opus") == 0 || strcasecmp(ext, "amr") == 0 ||
        strcasecmp(ext, "aac") == 0 || strcasecmp(ext, "m4a") == 0 ||
        strcasecmp(ext, "ogg") == 0) return "audio";
    if (strcasecmp(ext, "mp4") == 0 || strcasecmp(ext, "mov") == 0 ||
        strcasecmp(ext, "mkv") == 0 || strcasecmp(ext, "avi") == 0 ||
        strcasecmp(ext, "webm") == 0) return "video";
    return NULL;
}

static bool parse_attachment_spec(const char* raw, DingTalkAttachment* out) {
    if (!raw || !out) return false;
    memset(out, 0, sizeof(*out));

    if (raw[0] == '{') {
        cJSON* json = cJSON_Parse(raw);
        if (!json) return false;
        cJSON* type = cJSON_GetObjectItem(json, "type");
        cJSON* path = cJSON_GetObjectItem(json, "path");
        cJSON* duration = cJSON_GetObjectItem(json, "duration");
        cJSON* url = cJSON_GetObjectItem(json, "url");
        if (!cJSON_IsString(type) || !type->valuestring || !cJSON_IsString(path) || !path->valuestring) {
            cJSON_Delete(json);
            return false;
        }
        snprintf(out->type, sizeof(out->type), "%s", type->valuestring);
        snprintf(out->path, sizeof(out->path), "%s", path->valuestring);
        if (cJSON_IsString(url) && url->valuestring) {
            snprintf(out->url, sizeof(out->url), "%s", url->valuestring);
        }
        if (cJSON_IsNumber(duration) && duration->valueint > 0) {
            out->duration = duration->valueint;
        }
        cJSON_Delete(json);
        return true;
    }

    const char* inferred = infer_attachment_type(raw);
    if (!inferred) return false;
    snprintf(out->type, sizeof(out->type), "%s", inferred);
    snprintf(out->path, sizeof(out->path), "%s", raw);
    return true;
}

static bool build_upload_body(const char* boundary, const char* file_path, struct MemoryStruct* body) {
    unsigned char* file_bytes = NULL;
    size_t file_len = 0;
    if (!read_file_bytes(file_path, &file_bytes, &file_len)) return false;

    char line[2048];
    if (snprintf(line, sizeof(line), "--%s\r\n", boundary) < 0 || !append_text_chunk(body, line)) goto fail;
    if (snprintf(line, sizeof(line),
                 "Content-Disposition: form-data; name=\"media\"; filename=\"%s\"\r\n"
                 "Content-Type: application/octet-stream\r\n\r\n",
                 dingtalk_file_basename(file_path)) < 0 || !append_text_chunk(body, line)) goto fail;
    if (!memory_append_chunk(body, (const char*)file_bytes, file_len)) goto fail;
    if (!append_text_chunk(body, "\r\n")) goto fail;
    if (snprintf(line, sizeof(line), "--%s--\r\n", boundary) < 0 || !append_text_chunk(body, line)) goto fail;

    free(file_bytes);
    return true;
fail:
    free(file_bytes);
    return false;
}

static bool dingtalk_upload_attachment(DingTalkChannelData* data, const DingTalkAttachment* attachment, char** out_media_id) {
    const char* media_type = "image";
    if (strcmp(attachment->type, "audio") == 0) media_type = "voice";
    if (strcmp(attachment->type, "video") == 0) media_type = "video";

    log_debug("[DingTalk] Uploading attachment: path=%s, type=%s", attachment->path, media_type);

    char url[1024];
    snprintf(url, sizeof(url), "https://oapi.dingtalk.com/media/upload?access_token=%s&type=%s",
             data->access_token, media_type);

    const char* boundary = "----PrimagenDingTalkBoundary";
    struct MemoryStruct body = {0};
    body.memory = malloc(1);
    body.memory[0] = '\0';
    if (!build_upload_body(boundary, attachment->path, &body)) {
        free(body.memory);
        log_error("[DingTalk] Failed to build upload body: %s", attachment->path);
        return false;
    }

    log_debug("[DingTalk] Upload body built, size=%zu", body.size);

    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';
    mg_mgr_init(&mgr);
    apply_dns_config(&mgr, data);

    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        log_error("[DingTalk] Failed to connect to upload URL");
        mg_mgr_free(&mgr);
        free(body.memory);
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
        "Content-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %d\r\n"
        "\r\n",
        mg_url_uri(url),
        (int)host.len, host.buf,
        boundary,
        (int)body.size
    );
    mg_send(c, body.memory, body.size);
    while (!chunk.done) mg_mgr_poll(&mgr, 1000);

    bool ok = false;
    if (chunk.size > 0) {
        log_debug("[DingTalk] Upload response: %s", chunk.memory);
        cJSON* resp = cJSON_Parse(chunk.memory);
        if (resp) {
            cJSON* errcode = cJSON_GetObjectItem(resp, "errcode");
            cJSON* errmsg = cJSON_GetObjectItem(resp, "errmsg");
            if (errcode && errcode->valueint != 0) {
                log_error("[DingTalk] Upload failed: errcode=%d, errmsg=%s", 
                          errcode->valueint, errmsg ? errmsg->valuestring : "unknown");
            }
            if ((!errcode || errcode->valueint == 0)) {
                cJSON* media_id = cJSON_GetObjectItem(resp, "media_id");
                if (cJSON_IsString(media_id) && media_id->valuestring) {
                    *out_media_id = strdup(media_id->valuestring);
                    ok = *out_media_id != NULL;
                    log_debug("[DingTalk] Upload success: media_id=%s", *out_media_id);
                }
            }
            cJSON_Delete(resp);
        }
    } else {
        log_error("[DingTalk] Upload failed: no response received");
    }

    mg_mgr_free(&mgr);
    free(body.memory);
    free(chunk.memory);
    return ok;
}

static bool dingtalk_send_attachment_message(DingTalkChannelData* data, const char* conversation_id,
                                             const DingTalkAttachment* attachment) {
    char* media_id = NULL;
    if (!dingtalk_upload_attachment(data, attachment, &media_id)) return false;

    char url[1024];
    snprintf(url, sizeof(url), "https://oapi.dingtalk.com/chat/send?access_token=%s", data->access_token);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "chatid", conversation_id);
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "msg", msg);

    if (strcmp(attachment->type, "image") == 0) {
        cJSON_AddStringToObject(msg, "msgtype", "image");
        cJSON* image = cJSON_CreateObject();
        cJSON_AddStringToObject(image, "media_id", media_id);
        cJSON_AddItemToObject(msg, "image", image);
    } else if (strcmp(attachment->type, "audio") == 0) {
        cJSON_AddStringToObject(msg, "msgtype", "voice");
        cJSON* voice = cJSON_CreateObject();
        cJSON_AddStringToObject(voice, "media_id", media_id);
        if (attachment->duration > 0) {
            cJSON_AddNumberToObject(voice, "duration", attachment->duration);
        }
        cJSON_AddItemToObject(msg, "voice", voice);
    } else {
        cJSON_AddStringToObject(msg, "msgtype", "video");
        cJSON* video = cJSON_CreateObject();
        cJSON_AddStringToObject(video, "media_id", media_id);
        cJSON_AddItemToObject(msg, "video", video);
    }

    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(media_id);
    if (!body) return false;

    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';
    mg_mgr_init(&mgr);
    apply_dns_config(&mgr, data);

    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        mg_mgr_free(&mgr);
        free(chunk.memory);
        free(body);
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
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        mg_url_uri(url),
        (int)host.len, host.buf,
        (int)strlen(body),
        body
    );
    while (!chunk.done) mg_mgr_poll(&mgr, 1000);

    bool ok = false;
    if (chunk.size > 0) {
        cJSON* resp = cJSON_Parse(chunk.memory);
        if (resp) {
            cJSON* errcode = cJSON_GetObjectItem(resp, "errcode");
            ok = !errcode || errcode->valueint == 0;
            cJSON_Delete(resp);
        }
    }

    mg_mgr_free(&mgr);
    free(chunk.memory);
    free(body);
    return ok;
}

// =============================================================================
// WebSocket Message Callback
// =============================================================================

static void on_dingtalk_message(const char* conversation_id, const char* content,
                                 const char* sender_id, const char* session_webhook,
                                 void* user_data) {
    (void)sender_id;
    DingTalkChannelData* data = (DingTalkChannelData*)user_data;

    // Log session webhook for debugging
    if (session_webhook) {
        log_info("[DingTalk] Session webhook received: %s", session_webhook);
    } else {
        log_warn("[DingTalk] No session webhook in CALLBACK message");
    }

    // Send to message bus - include session_webhook in chat_id field for reply routing
    // Format: "sessionWebhookURL|conversationId"
    char* routing_info = NULL;
    if (session_webhook && session_webhook[0] != '\0') {
        size_t len = strlen(session_webhook) + strlen(conversation_id) + 2;
        routing_info = malloc(len);
        snprintf(routing_info, len, "%s|%s", session_webhook, conversation_id);
        log_info("[DingTalk] Created routing info: %s", routing_info);
    }

    const char* final_chat_id = routing_info ? routing_info : conversation_id;
    log_info("[DingTalk] Sending to message bus with chat_id: %s", final_chat_id);

    InboundMessage* inbound = inbound_message_new("dingtalk", final_chat_id, content);
    message_bus_send_inbound(data->bus, inbound);

    if (routing_info) free(routing_info);
}

// =============================================================================
// WebSocket Receive Loop
// =============================================================================

static void* dingtalk_receive_loop(void* arg) {
    DingTalkChannelData* data = (DingTalkChannelData*)arg;
    int reconnect_attempt = 0;

    while (data->running) {
        if (!is_token_valid(data)) {
            refresh_token(data);
        }
        if (!data->access_token || data->access_token[0] == '\0') {
            reconnect_attempt++;
            int delay = reconnect_attempt < 6 ? (1 << reconnect_attempt) : 60;
            if (delay > 60) delay = 60;
            log_error("[DingTalk] Missing access token, reconnect in %ds (attempt %d)", delay, reconnect_attempt);
            if (data->running) sleep(delay);
            continue;
        }

        // Get WebSocket URL from DingTalk
        char* ws_url = dingtalk_get_ws_url(data->client_id,
                                           data->client_secret,
                                           data->access_token,
                                           data->dns4,
                                           data->dns6,
                                           data->dns_timeout_ms,
                                           data->use_system_resolver);

        if (ws_url) {
            log_info("[DingTalk] Connecting to WebSocket...");
            data->ws = dingtalk_ws_create();
            dingtalk_ws_set_dns(data->ws, data->dns4, data->dns6, data->dns_timeout_ms, data->use_system_resolver);

            if (dingtalk_ws_connect(data->ws, ws_url, data->access_token,
                                    data->client_secret)) {
                log_info("[DingTalk] WebSocket connected.");
                reconnect_attempt = 0;
                // Run WebSocket loop (blocks until disconnected)
                dingtalk_ws_run(data->ws, on_dingtalk_message, data);
                if (data->running) {
                    reconnect_attempt++;
                    int delay = reconnect_attempt < 6 ? (1 << reconnect_attempt) : 60;
                    if (delay > 60) delay = 60;
                    log_warn("[DingTalk] WebSocket disconnected, reconnect in %ds (attempt %d)", delay, reconnect_attempt);
                    sleep(delay);
                }
            } else {
                reconnect_attempt++;
                int delay = reconnect_attempt < 6 ? (1 << reconnect_attempt) : 60;
                if (delay > 60) delay = 60;
                log_error("[DingTalk] WebSocket connect failed.");
                if (data->running) {
                    log_warn("[DingTalk] Reconnect in %ds (attempt %d)", delay, reconnect_attempt);
                    sleep(delay);
                }
            }

            dingtalk_ws_destroy(data->ws);
            data->ws = NULL;
            free(ws_url);
        } else {
            reconnect_attempt++;
            int delay = reconnect_attempt < 6 ? (1 << reconnect_attempt) : 60;
            if (delay > 60) delay = 60;
            log_error("[DingTalk] Failed to get WebSocket URL");
            refresh_token(data);
            if (data->running) {
                log_warn("[DingTalk] Reconnect in %ds (attempt %d)", delay, reconnect_attempt);
                sleep(delay);
            }
        }
    }

    return NULL;
}

// =============================================================================
// Channel Implementation
// =============================================================================

static bool dingtalk_init(Channel* self, Config* config, MessageBus* bus) {
    DingTalkChannelData* data = malloc(sizeof(DingTalkChannelData));
    if (!data) return false;

    data->bus = bus;
    data->running = false;
    data->access_token = NULL;
    data->token_expiry = 0;
    data->thread_id = 0;
    data->ws = NULL;
    data->client_id = NULL;
    data->client_secret = NULL;
    data->dns4 = NULL;
    data->dns6 = NULL;
    data->dns_timeout_ms = 0;
    data->use_system_resolver = false;

    // Get plugin configuration
    PluginConfig* plugin_cfg = config_get_plugin_config(config, "dingtalk_channel");
    if (plugin_cfg && plugin_cfg->config) {
        cJSON* client_id = cJSON_GetObjectItem(plugin_cfg->config, "clientId");
        cJSON* client_secret = cJSON_GetObjectItem(plugin_cfg->config, "clientSecret");

        data->client_id = client_id && cJSON_IsString(client_id) ? strdup(client_id->valuestring) : strdup("");
        data->client_secret = client_secret && cJSON_IsString(client_secret) ? strdup(client_secret->valuestring) : strdup("");
    } else {
        data->client_id = strdup("");
        data->client_secret = strdup("");
    }

    DNSConfig* dns_cfg = config_get_dns_config(config);
    if (dns_cfg) {
        if (dns_cfg->dns4 && dns_cfg->dns4[0]) data->dns4 = strdup(dns_cfg->dns4);
        if (dns_cfg->dns6 && dns_cfg->dns6[0]) data->dns6 = strdup(dns_cfg->dns6);
        if (dns_cfg->dns_timeout_ms > 0) data->dns_timeout_ms = dns_cfg->dns_timeout_ms;
        data->use_system_resolver = dns_cfg->use_system_resolver;
    }

    self->user_data = data;
    log_info("[DingTalk] Initialized with client_id: %s", data->client_id);
    return true;
}

static void dingtalk_start(Channel* self) {
    DingTalkChannelData* data = (DingTalkChannelData*)self->user_data;
    if (data->running) return;
    if (!data->client_id || !data->client_id[0] || !data->client_secret || !data->client_secret[0]) {
        log_info("[DingTalk] Channel not started (missing credentials)");
        return;
    }

    log_info("[DingTalk] Starting channel...");
    data->running = true;

    // Get initial access token
    refresh_token(data);

    // Start receive thread
    pthread_create(&data->thread_id, NULL, dingtalk_receive_loop, data);

    log_info("[DingTalk] Channel started.");
}

static void dingtalk_stop(Channel* self) {
    DingTalkChannelData* data = (DingTalkChannelData*)self->user_data;
    data->running = false;

    // Stop WebSocket connection
    if (data->ws) {
        dingtalk_ws_stop(data->ws);
    }

    // Wait for thread to finish
    if (data->thread_id != 0) {
        pthread_join(data->thread_id, NULL);
        data->thread_id = 0;
    }
}

static void dingtalk_send(Channel* self, OutboundMessage* msg) {
    DingTalkChannelData* data = (DingTalkChannelData*)self->user_data;
    if (!data->client_id || !data->client_id[0] || !data->client_secret || !data->client_secret[0]) return;

    // Only process messages meant for this channel
    if (strcmp(msg->channel.data, "dingtalk") != 0) return;

    // Parse chat_id to check for session webhook format: "webhookURL|conversationId"
    const char* session_webhook = NULL;
    const char* conversation_id = NULL;
    char* webhook_buf = NULL;  // Track allocated buffer for cleanup
    char* pipe_pos = strchr(msg->chat_id.data, '|');

    if (pipe_pos) {
        // Session webhook format - extract webhook URL and conversation ID
        // Copy webhook URL (before |) to a buffer
        size_t webhook_len = pipe_pos - msg->chat_id.data;
        webhook_buf = malloc(webhook_len + 1);
        if (!webhook_buf) {
            log_error("[DingTalk] Failed to allocate webhook buffer");
            return;
        }
        memcpy(webhook_buf, msg->chat_id.data, webhook_len);
        webhook_buf[webhook_len] = '\0';
        session_webhook = webhook_buf;
        conversation_id = pipe_pos + 1;
        log_debug("[DingTalk] Using session webhook: %s for conversation: %s",
                  session_webhook, conversation_id);
    } else {
        // Legacy format - just conversation ID (cannot reply effectively)
        conversation_id = msg->chat_id.data;
        log_warn("[DingTalk] No session webhook available for %s, reply may fail", conversation_id);
    }

    // Ensure we have a valid token
    if (!is_token_valid(data)) {
        refresh_token(data);
    }

    if (!data->access_token) {
        log_error("[DingTalk] Cannot send: no access token");
        free(webhook_buf);
        return;
    }

    bool has_text = msg->content.data && msg->content.data[0] != '\0';
    char* merged_content = NULL;

    if (msg->attachments.count > 0) {
        for (size_t i = 0; i < msg->attachments.count; i++) {
            DingTalkAttachment attachment;
            if (!parse_attachment_spec(msg->attachments.items[i].data, &attachment)) {
                log_error("[DingTalk] Invalid attachment spec: %s", msg->attachments.items[i].data);
                continue;
            }
            if (attachment.url[0] != '\0') {
                char markdown_line[4096];
                if (snprintf(markdown_line, sizeof(markdown_line), "![image](%s)", attachment.url) > 0) {
                    if (!merged_content) {
                        merged_content = strdup(has_text ? msg->content.data : "");
                    }
                    char* next = realloc(merged_content, strlen(merged_content) + strlen(markdown_line) + 3);
                    if (next) {
                        strcat(next, "\n\n");
                        strcat(next, markdown_line);
                        merged_content = next;
                        has_text = true;
                    }
                }
            }
        }
    }

    if (!has_text) {
        free(merged_content);
        free(webhook_buf);
        return;
    }

    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';

    mg_mgr_init(&mgr);

    apply_dns_config(&mgr, data);

    if (session_webhook && strncmp(session_webhook, "http", 4) == 0) {
        log_info("[DingTalk] Sending via session webhook to %s", conversation_id);

        const char* content = merged_content ? merged_content : msg->content.data;
        while (*content == ' ' || *content == '\n' || *content == '\r') content++;
        log_info("[DingTalk] Reply content: %s", content);

        cJSON* json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "msgtype", "markdown");
        cJSON* markdown = cJSON_CreateObject();
        cJSON_AddStringToObject(markdown, "title", "Primagen");
        cJSON_AddStringToObject(markdown, "text", content);
        cJSON_AddItemToObject(json, "markdown", markdown);
        char* json_str = cJSON_PrintUnformatted(json);

        log_debug("[DingTalk] Request body: %s", json_str);

        struct mg_connection *c = mg_http_connect(&mgr, session_webhook, webhook_fn, &chunk);
        if (!c) {
            log_error("[DingTalk] Webhook send failed: connection error");
            free(json_str);
            cJSON_Delete(json);
            free(merged_content);
            mg_mgr_free(&mgr);
            free(chunk.memory);
            free(webhook_buf);
            return;
        }

        struct mg_str host = mg_url_host(session_webhook);

        struct mg_tls_opts opts = {0};
        opts.ca = mg_str("");
        opts.name = host;
        opts.skip_verification = true;
        if (mg_url_is_ssl(session_webhook)) {
            mg_tls_init(c, &opts);
        }

        mg_printf(c,
            "POST %s HTTP/1.1\r\n"
            "Host: %.*s\r\n"
            "Content-Type: application/json\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            mg_url_uri(session_webhook),
            (int)host.len, host.buf,
            (int)strlen(json_str),
            json_str
        );

        log_info("[DingTalk] Full request sent: POST %s HTTP/1.1", mg_url_uri(session_webhook));

        int timeout_count = 0;
        const int max_timeout = 30;
        while (!chunk.done && timeout_count < max_timeout) {
            mg_mgr_poll(&mgr, 1000);
            timeout_count++;
        }

        if (timeout_count >= max_timeout) {
            log_error("[DingTalk] Webhook send timeout");
            c->is_closing = 1;
        }

        if (chunk.size > 0) {
            log_info("[DingTalk] Webhook response (%zu bytes): %s", chunk.size, chunk.memory);

            char hex_buf[256];
            size_t hex_len = chunk.size > 64 ? 64 : chunk.size;
            for (size_t i = 0; i < hex_len; i++) {
                snprintf(hex_buf + (i * 3), 4, "%02x ", (unsigned char)chunk.memory[i]);
            }
            hex_buf[hex_len * 3] = '\0';
            log_info("[DingTalk] Webhook response hex: %s", hex_buf);

            cJSON* resp = cJSON_Parse(chunk.memory);
            if (resp) {
                cJSON* errcode = cJSON_GetObjectItem(resp, "errcode");
                if (errcode && cJSON_IsNumber(errcode)) {
                    log_debug("[DingTalk] Response errcode: %d", errcode->valueint);
                    if (errcode->valueint == 0) {
                        log_info("[DingTalk] Sent via webhook to %s", conversation_id);
                    } else {
                        log_error("[DingTalk] Webhook send failed with errcode %d: %s", errcode->valueint, chunk.memory);
                    }
                } else if (errcode && !cJSON_IsNumber(errcode)) {
                    log_error("[DingTalk] Webhook send failed: errcode is not a number: %s", chunk.memory);
                } else {
                    log_warn("[DingTalk] Webhook response missing errcode field: %s", chunk.memory);
                    log_info("[DingTalk] Sent via webhook to %s (no errcode, assuming success)", conversation_id);
                }
                cJSON_Delete(resp);
            } else {
                log_error("[DingTalk] Failed to parse webhook response as JSON: %s", chunk.memory);
            }
        } else {
            log_error("[DingTalk] Webhook send failed: empty response");
        }

        cJSON_Delete(json);
        free(json_str);
        free(merged_content);
        mg_mgr_free(&mgr);
        free(chunk.memory);
        free(webhook_buf);

    } else {
        log_warn("[DingTalk] Falling back to batchSend API (requires staff IDs)");

        const char* url = "https://api.dingtalk.com/v1.0/robot/oToMessages/batchSend";

        cJSON* json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "robotCode", data->client_id);

        cJSON* userIds = cJSON_CreateArray();
        cJSON_AddItemToArray(userIds, cJSON_CreateString(msg->chat_id.data));
        cJSON_AddItemToObject(json, "userIds", userIds);

        cJSON_AddStringToObject(json, "msgKey", "sampleMarkdown");

        cJSON* msgParam = cJSON_CreateObject();
        cJSON_AddStringToObject(msgParam, "markdown", merged_content ? merged_content : msg->content.data);
        cJSON_AddStringToObject(msgParam, "title", "Primagen");
        char* param_str = cJSON_PrintUnformatted(msgParam);
        cJSON_AddStringToObject(json, "msgParam", param_str);
        free(param_str);
        cJSON_Delete(msgParam);

        char* json_str = cJSON_PrintUnformatted(json);

        struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
        if (!c) {
            log_error("[DingTalk] Send failed: connection error");
            free(json_str);
            cJSON_Delete(json);
            free(merged_content);
            mg_mgr_free(&mgr);
            free(chunk.memory);
            free(webhook_buf);
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
            "x-acs-dingtalk-access-token: %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            mg_url_uri(url),
            (int)host.len, host.buf,
            data->access_token,
            (int)strlen(json_str),
            json_str
        );

        while (!chunk.done) mg_mgr_poll(&mgr, 1000);

        if (chunk.size > 0) {
            cJSON* resp = cJSON_Parse(chunk.memory);
            if (resp) {
                cJSON* code = cJSON_GetObjectItem(resp, "code");
                if (code && code->valueint == 0) {
                    log_info("[DingTalk] Sent to %s", msg->chat_id.data);
                    char* resp_str = cJSON_PrintUnformatted(resp);
                    log_debug("[DingTalk] Send response: %s", resp_str);
                    free(resp_str);
                } else {
                    log_error("[DingTalk] Send failed: %s", chunk.memory);
                }
                cJSON_Delete(resp);
            }
        } else {
            log_error("[DingTalk] Send failed: empty response");
        }

        cJSON_Delete(json);
        free(json_str);
        free(merged_content);
        mg_mgr_free(&mgr);
        free(chunk.memory);
        free(webhook_buf);
    }
}

static void dingtalk_destroy(Channel* self) {
    DingTalkChannelData* data = (DingTalkChannelData*)self->user_data;
    if (data) {
        if (data->access_token) free(data->access_token);
        if (data->client_id) free(data->client_id);
        if (data->client_secret) free(data->client_secret);
        if (data->dns4) free(data->dns4);
        if (data->dns6) free(data->dns6);
        if (data->ws) {
            dingtalk_ws_destroy(data->ws);
        }
        free(data);
    }
    log_info("[DingTalkChannel] Destroyed");
    free(self);
}

// =============================================================================
// Channel Factory
// =============================================================================

static Channel* dingtalk_channel_create(void) {
    Channel* channel = calloc(1, sizeof(Channel));
    if (!channel) return NULL;

    channel->name = strdup("dingtalk");
    channel->init = dingtalk_init;
    channel->start = dingtalk_start;
    channel->stop = dingtalk_stop;
    channel->send = dingtalk_send;
    channel->destroy = dingtalk_destroy;
    channel->user_data = NULL;
    channel->plugin_ref = NULL;

    log_info("[DingTalkChannel] Created new channel instance");
    return channel;
}

// =============================================================================
// Plugin Initialization
// =============================================================================

PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context) {
    (void)context;

    log_info("[Plugin:dingtalk_channel] Initializing dingtalk channel plugin");

    // Register the channel factory
    int ret = plugin_register_channel(manager, NULL, "dingtalk", dingtalk_channel_create);

    if (ret == 0) {
        log_info("[Plugin:dingtalk_channel] Successfully registered dingtalk channel");
    } else {
        log_error("[Plugin:dingtalk_channel] Failed to register dingtalk channel");
        return -1;
    }

    return 0;
}

PLUGIN_EXPORT int plugin_cleanup(void) {
    log_info("[Plugin:dingtalk_channel] Cleaning up dingtalk channel plugin");
    return 0;
}

// =============================================================================
// Plugin Information
// =============================================================================

static PluginInfo g_plugin_info = {
    .version = 1,
    .type = PLUGIN_CHANNEL,
    .name = "dingtalk_channel",
    .description = "DingTalk channel support for Primagen",
    .plugin_id = "dingtalk_channel"
};

PLUGIN_EXPORT PluginInfo* plugin_get_info(void) {
    return &g_plugin_info;
}
