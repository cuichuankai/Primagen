/*
 * DingTalk Channel Plugin
 *
 * A plugin that provides DingTalk channel support for Primagen.
 * Uses DingTalk API for sending and receiving messages.
 *
 * Configuration (.primagen/config.json):
 *   "channels": {
 *     "dingtalk": {
 *       "enabled": true,
 *       "clientId": "your_robot_client_id",
 *       "clientSecret": "your_robot_client_secret",
 *       "allowFrom": ["user1", "user2"]
 *     }
 *   }
 *
 * Receiving Messages (WebSocket Mode):
 *   The channel automatically connects to DingTalk WebSocket server
 *   for real-time message reception.
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
#include <pthread.h>
#include <unistd.h>

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
    // Session webhook cache (for replying to conversations)
    SessionWebhook* session_cache;
    size_t session_cache_size;
} DingTalkChannelData;

struct MemoryStruct {
    char *memory;
    size_t size;
    bool done;
};

// =============================================================================
// HTTP Callback for Mongoose
// =============================================================================

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

// Separate callback for webhook sends that handles responses without Content-Length
// DingTalk session webhook responses don't include Content-Length, so we accumulate
// data as it arrives via MG_EV_READ and mark done on MG_EV_CLOSE
static void webhook_fn(struct mg_connection *c, int ev, void *ev_data) {
    struct MemoryStruct *ms = (struct MemoryStruct *) c->fn_data;

    if (ev == MG_EV_HTTP_MSG) {
        // Normal case: response has Content-Length or is chunked
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        if (hm->body.len > 0) {
            size_t new_size = ms->size + hm->body.len;
            ms->memory = realloc(ms->memory, new_size + 1);
            if (ms->memory) {
                memcpy(ms->memory + ms->size, hm->body.buf, hm->body.len);
                ms->size = new_size;
                ms->memory[ms->size] = '\0';
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
                size_t new_size = ms->size + bytes_read;
                ms->memory = realloc(ms->memory, new_size + 1);
                if (ms->memory) {
                    memcpy(ms->memory + ms->size, new_data, bytes_read);
                    ms->size = new_size;
                    ms->memory[ms->size] = '\0';
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

    // Configure DNS explicitly to avoid DNS timeout issues
    mgr.dns4.url = "udp://8.8.8.8:53";
    mgr.dns6.url = "udp://[2001:4860:4860::8888]:53";
    mgr.dnstimeout = 10000;  // 10 seconds timeout

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
                                           data->access_token);

        if (ws_url) {
            log_info("[DingTalk] Connecting to WebSocket...");
            data->ws = dingtalk_ws_create();

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
        return;
    }

    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';

    mg_mgr_init(&mgr);

    // Configure DNS explicitly to avoid DNS timeout issues
    mgr.dns4.url = "udp://8.8.8.8:53";
    mgr.dns6.url = "udp://[2001:4860:4860::8888]:53";
    mgr.dnstimeout = 10000;  // 10 seconds timeout

    if (session_webhook && strncmp(session_webhook, "http", 4) == 0) {
        // Use session webhook to send message (preferred method for replies)
        // Try text format first for compatibility
        log_info("[DingTalk] Sending via session webhook to %s", conversation_id);

        // Trim leading/trailing whitespace from content
        const char* content = msg->content.data;
        while (*content == ' ' || *content == '\n' || *content == '\r') content++;
        log_info("[DingTalk] Reply content: %s", content);

        // Use markdown format for better readability
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

        // Poll with timeout to prevent hanging
        int timeout_count = 0;
        const int max_timeout = 30; // 30 seconds max
        while (!chunk.done && timeout_count < max_timeout) {
            mg_mgr_poll(&mgr, 1000);
            timeout_count++;
        }

        if (timeout_count >= max_timeout) {
            log_error("[DingTalk] Webhook send timeout");
            c->is_closing = 1;
        }

        if (chunk.size > 0) {
            // Log raw response for debugging (hex dump for non-printable chars)
            log_info("[DingTalk] Webhook response (%zu bytes): %s", chunk.size, chunk.memory);

            // Also log hex dump to see any hidden characters
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
                    // Assume success if status 200 and no errcode (like Go SDK)
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
        mg_mgr_free(&mgr);
        free(chunk.memory);
        free(webhook_buf);

    } else {
        // Fallback: use batchSend API (requires user staff IDs, not conversation ID)
        // This path is kept for compatibility but won't work with conversation IDs
        log_warn("[DingTalk] Falling back to batchSend API (requires staff IDs)");

        const char* url = "https://api.dingtalk.com/v1.0/robot/oToMessages/batchSend";

        cJSON* json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "robotCode", data->client_id);

        cJSON* userIds = cJSON_CreateArray();
        cJSON_AddItemToArray(userIds, cJSON_CreateString(msg->chat_id.data));
        cJSON_AddItemToObject(json, "userIds", userIds);

        // Use sampleMarkdown type with correct fields
        cJSON_AddStringToObject(json, "msgKey", "sampleMarkdown");

        cJSON* msgParam = cJSON_CreateObject();
        cJSON_AddStringToObject(msgParam, "markdown", msg->content.data);
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
                    // Log full response for debugging
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
        mg_mgr_free(&mgr);
        free(chunk.memory);
        free(webhook_buf);
    }
}

static void dingtalk_destroy(Channel* self) {
    DingTalkChannelData* data = (DingTalkChannelData*)self->user_data;
    if (data) {
        if (data->access_token) free(data->access_token);
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
