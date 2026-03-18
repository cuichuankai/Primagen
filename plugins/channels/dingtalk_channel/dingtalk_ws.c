/*
 * DingTalk WebSocket Client
 * Implements the DingTalk stream protocol with Protobuf framing
 * Based on: https://github.com/open-dingtalk/dingtalk-stream-sdk-go
 */

#include "dingtalk_ws.h"
#include "../../../src/include/logger.h"
#include "../../../src/vendor/cJSON/cJSON.h"
#include "../../../src/vendor/mongoose/mongoose.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>

// Protocol constants
#define DINGTALK_WS_VERSION     "1.0"
#define DINGTALK_WS_SERVICE_ID  1  // IM service
#define DINGTALK_WS_METHOD      1  // Message method

// Frame wire types for Protobuf
#define WIRE_TYPE_VARINT        0
#define WIRE_TYPE_FIXED64       1
#define WIRE_TYPE_LENGTH_DELIM  2
#define WIRE_TYPE_FIXED32       5

// Frame field numbers
#define FRAME_FIELD_SEQ_ID      1
#define FRAME_FIELD_LOG_ID      2
#define FRAME_FIELD_SERVICE     3
#define FRAME_FIELD_METHOD      4
#define FRAME_FIELD_HEADERS     5
#define FRAME_FIELD_PAYLOAD_TYPE 7
#define FRAME_FIELD_PAYLOAD     8

// Header field numbers
#define HEADER_FIELD_KEY        1
#define HEADER_FIELD_VALUE      2

// Message types
#define MSG_TYPE_PING           "ping"
#define MSG_TYPE_PONG           "pong"
#define MSG_TYPE_EVENT          "event"
#define MSG_TYPE_ACK            "ack"

typedef struct {
    char* key;
    char* value;
} WSHeader;

typedef struct {
    uint64_t seq_id;
    uint64_t log_id;
    int32_t service;
    int32_t method;
    WSHeader headers[64];
    size_t headers_count;
    char* payload_type;
    unsigned char* payload;
    size_t payload_len;
} WSFrame;

typedef struct {
    unsigned char* data;
    size_t len;
    size_t cap;
} ByteBuf;

struct DingTalkWS {
    struct mg_mgr mgr;
    struct mg_connection* c;
    bool running;
    DingTalkWSMessageCallback callback;
    void* user_data;
    char* access_token;
    char* ws_url;
    uint64_t ping_interval_ms;
    uint64_t next_ping_ms;
    char* recent_msg_ids[20];  // For deduplication
    int msg_id_idx;
};

// =============================================================================
// Utility Functions
// =============================================================================

static uint64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

static uint64_t read_varint(const unsigned char* data, size_t len, size_t* offset) {
    uint64_t value = 0;
    int shift = 0;
    while (*offset < len) {
        unsigned char b = data[*offset];
        (*offset)++;
        value |= ((uint64_t)(b & 0x7F)) << shift;
        if ((b & 0x80) == 0) break;
        shift += 7;
    }
    return value;
}

static void skip_field(const unsigned char* data, size_t len, size_t* offset, int wire_type) {
    if (wire_type == 0) {
        read_varint(data, len, offset);
    } else if (wire_type == 2) {
        uint64_t l = read_varint(data, len, offset);
        if (*offset + l > len)
            *offset = len;
        else
            *offset += l;
    } else if (wire_type == 1) {
        *offset += 8;
    } else if (wire_type == 5) {
        *offset += 4;
    }
    if (*offset > len) *offset = len;
}

// =============================================================================
// Byte Buffer Operations
// =============================================================================

static bool bb_ensure(ByteBuf* b, size_t extra) {
    size_t need = b->len + extra;
    size_t ncap = b->cap ? b->cap : 256;
    unsigned char* p;
    if (need <= b->cap) return true;
    while (ncap < need) ncap *= 2;
    p = (unsigned char*)realloc(b->data, ncap);
    if (!p) return false;
    b->data = p;
    b->cap = ncap;
    return true;
}

static bool bb_add(ByteBuf* b, const void* p, size_t n) {
    if (!bb_ensure(b, n)) return false;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return true;
}

static bool bb_add_u8(ByteBuf* b, uint8_t v) { return bb_add(b, &v, 1); }

static bool bb_add_varint(ByteBuf* b, uint64_t v) {
    while (v >= 0x80) {
        uint8_t c = (uint8_t)((v & 0x7f) | 0x80);
        if (!bb_add_u8(b, c)) return false;
        v >>= 7;
    }
    return bb_add_u8(b, (uint8_t)v);
}

static bool pb_add_key(ByteBuf* b, int field, int wt) {
    return bb_add_varint(b, ((uint64_t)field << 3) | (uint64_t)wt);
}

static bool pb_add_bytes(ByteBuf* b, int field, const void* p, size_t n) {
    if (!pb_add_key(b, field, 2)) return false;
    if (!bb_add_varint(b, n)) return false;
    return n == 0 ? true : bb_add(b, p, n);
}

static bool pb_add_varint_field(ByteBuf* b, int field, uint64_t v) {
    if (!pb_add_key(b, field, 0)) return false;
    return bb_add_varint(b, v);
}

// =============================================================================
// Frame Operations
// =============================================================================

static const char* frame_header_get(WSFrame* f, const char* k) {
    size_t i;
    for (i = 0; i < f->headers_count; i++) {
        if (f->headers[i].key && strcmp(f->headers[i].key, k) == 0)
            return f->headers[i].value;
    }
    return NULL;
}

static int frame_header_get_int(WSFrame* f, const char* k) {
    const char* v = frame_header_get(f, k);
    return v ? atoi(v) : 0;
}

static bool frame_header_set(WSFrame* f, const char* k, const char* v) {
    size_t i;
    for (i = 0; i < f->headers_count; i++) {
        if (f->headers[i].key && strcmp(f->headers[i].key, k) == 0) {
            char* nv = strdup(v ? v : "");
            if (!nv) return false;
            free(f->headers[i].value);
            f->headers[i].value = nv;
            return true;
        }
    }
    if (f->headers_count >= sizeof(f->headers) / sizeof(f->headers[0]))
        return false;
    f->headers[f->headers_count].key = strdup(k);
    f->headers[f->headers_count].value = strdup(v ? v : "");
    if (!f->headers[f->headers_count].key || !f->headers[f->headers_count].value)
        return false;
    f->headers_count++;
    return true;
}

static void frame_free(WSFrame* f) {
    size_t i;
    for (i = 0; i < f->headers_count; i++) {
        free(f->headers[i].key);
        free(f->headers[i].value);
    }
    free(f->payload_type);
    free(f->payload);
    memset(f, 0, sizeof(*f));
}

// =============================================================================
// Protobuf Parsing
// =============================================================================

static bool parse_header_msg(const unsigned char* p, size_t n, WSHeader* h) {
    size_t off = 0;
    while (off < n) {
        uint64_t k = read_varint(p, n, &off);
        int f = (int)(k >> 3), wt = (int)(k & 7);
        if (wt == 2) {
            uint64_t l = read_varint(p, n, &off);
            if (off + l > n) return false;
            if (f == 1) {
                h->key = (char*)malloc((size_t)l + 1);
                if (!h->key) return false;
                memcpy(h->key, p + off, (size_t)l);
                h->key[l] = 0;
            } else if (f == 2) {
                h->value = (char*)malloc((size_t)l + 1);
                if (!h->value) return false;
                memcpy(h->value, p + off, (size_t)l);
                h->value[l] = 0;
            }
            off += (size_t)l;
        } else {
            skip_field(p, n, &off, wt);
        }
    }
    return true;
}

static bool parse_frame(const unsigned char* p, size_t n, WSFrame* f) {
    size_t off = 0;
    while (off < n) {
        uint64_t k = read_varint(p, n, &off);
        int fn = (int)(k >> 3), wt = (int)(k & 7);
        if (fn == 1 && wt == 0) {
            f->seq_id = read_varint(p, n, &off);
        } else if (fn == 2 && wt == 0) {
            f->log_id = read_varint(p, n, &off);
        } else if (fn == 3 && wt == 0) {
            f->service = (int32_t)read_varint(p, n, &off);
        } else if (fn == 4 && wt == 0) {
            f->method = (int32_t)read_varint(p, n, &off);
        } else if (fn == 5 && wt == 2) {
            uint64_t l = read_varint(p, n, &off);
            WSHeader h = {0};
            if (off + l > n) return false;
            if (f->headers_count >= sizeof(f->headers) / sizeof(f->headers[0]))
                return false;
            if (!parse_header_msg(p + off, (size_t)l, &h)) {
                free(h.key);
                free(h.value);
                return false;
            }
            f->headers[f->headers_count++] = h;
            off += (size_t)l;
        } else if (fn == 7 && wt == 2) {
            uint64_t l = read_varint(p, n, &off);
            if (off + l > n) return false;
            free(f->payload_type);
            f->payload_type = (char*)malloc((size_t)l + 1);
            if (!f->payload_type) return false;
            memcpy(f->payload_type, p + off, (size_t)l);
            f->payload_type[l] = 0;
            off += (size_t)l;
        } else if (fn == 8 && wt == 2) {
            uint64_t l = read_varint(p, n, &off);
            if (off + l > n) return false;
            free(f->payload);
            f->payload = (unsigned char*)malloc((size_t)l);
            if (l > 0 && !f->payload) return false;
            if (l > 0) memcpy(f->payload, p + off, (size_t)l);
            f->payload_len = (size_t)l;
            off += (size_t)l;
        } else {
            skip_field(p, n, &off, wt);
        }
    }
    return true;
}

static bool encode_frame(const WSFrame* f, ByteBuf* out) {
    size_t i;
    if (!pb_add_varint_field(out, 1, f->seq_id)) return false;
    if (!pb_add_varint_field(out, 2, f->log_id)) return false;
    if (!pb_add_varint_field(out, 3, (uint64_t)f->service)) return false;
    if (!pb_add_varint_field(out, 4, (uint64_t)f->method)) return false;
    for (i = 0; i < f->headers_count; i++) {
        ByteBuf hb = {0};
        if (!f->headers[i].key || !f->headers[i].value) continue;
        if (!pb_add_bytes(&hb, 1, f->headers[i].key, strlen(f->headers[i].key)) ||
            !pb_add_bytes(&hb, 2, f->headers[i].value, strlen(f->headers[i].value)) ||
            !pb_add_bytes(out, 5, hb.data, hb.len)) {
            free(hb.data);
            return false;
        }
        free(hb.data);
    }
    if (f->payload_type &&
        !pb_add_bytes(out, 7, f->payload_type, strlen(f->payload_type)))
        return false;
    if (!pb_add_bytes(out, 8, f->payload, f->payload_len)) return false;
    return true;
}

// =============================================================================
// Message Deduplication
// =============================================================================

static bool is_duplicate_event(DingTalkWS* ws, const char* event_id) {
    int i;
    if (!event_id) return false;
    for (i = 0; i < 20; i++) {
        if (ws->recent_msg_ids[i] &&
            strcmp(ws->recent_msg_ids[i], event_id) == 0)
            return true;
    }
    if (ws->recent_msg_ids[ws->msg_id_idx])
        free(ws->recent_msg_ids[ws->msg_id_idx]);
    ws->recent_msg_ids[ws->msg_id_idx] = strdup(event_id);
    ws->msg_id_idx = (ws->msg_id_idx + 1) % 20;
    return false;
}

// =============================================================================
// Event Handling
// =============================================================================

static void handle_event_json(DingTalkWS* ws, const char* event_json) {
    cJSON *root, *type_item, *headers, *data_item, *message_id;
    cJSON *parsed_data, *conversation_id, *text_content, *sender_id, *msg_type;
    cJSON *session_webhook_item;
    const char* type_str = NULL;
    const char* data_str = NULL;
    const char* session_webhook_str = NULL;

    if (!event_json) return;

    root = cJSON_Parse(event_json);
    if (!root) {
        log_error("[DingTalk] Failed to parse JSON: %s", event_json);
        return;
    }

    // Log full event for debugging
    char* debug_json = cJSON_PrintUnformatted(root);
    log_debug("[DingTalk] Full event: %s", debug_json ? debug_json : "null");
    free(debug_json);

    // DingTalk Stream protocol format:
    // {"specVersion":"1.0","type":"CALLBACK","headers":{...},"data":"<JSON string>"}
    type_item = cJSON_GetObjectItem(root, "type");
    if (!type_item || !cJSON_IsString(type_item)) {
        log_debug("[DingTalk] No 'type' field in message");
        cJSON_Delete(root);
        return;
    }
    type_str = type_item->valuestring;

    log_info("[DingTalk] Message type: %s", type_str);

    // Handle different message types
    if (strcmp(type_str, "CALLBACK") == 0) {
        // CALLBACK: bot message callbacks
        headers = cJSON_GetObjectItem(root, "headers");
        data_item = cJSON_GetObjectItem(root, "data");
        message_id = headers ? cJSON_GetObjectItem(headers, "messageId") : NULL;

        if (!data_item || !cJSON_IsString(data_item)) {
            log_error("[DingTalk] CALLBACK message has no 'data' field");
            cJSON_Delete(root);
            return;
        }
        data_str = data_item->valuestring;

        // Parse the data JSON string
        parsed_data = cJSON_Parse(data_str);
        if (!parsed_data) {
            log_error("[DingTalk] Failed to parse data JSON: %s", data_str);
            cJSON_Delete(root);
            return;
        }

        // Extract message fields from parsed data
        conversation_id = cJSON_GetObjectItem(parsed_data, "conversationId");
        text_content = cJSON_GetObjectItem(parsed_data, "text");
        sender_id = cJSON_GetObjectItem(parsed_data, "senderId");
        msg_type = cJSON_GetObjectItem(parsed_data, "msgtype");
        session_webhook_item = cJSON_GetObjectItem(parsed_data, "sessionWebhook");

        // Extract session webhook URL if present
        if (session_webhook_item && cJSON_IsString(session_webhook_item)) {
            session_webhook_str = session_webhook_item->valuestring;
            log_info("[DingTalk] Session webhook URL found: %s", session_webhook_str);
        } else {
            log_warn("[DingTalk] No sessionWebhook field in CALLBACK data");
            // Log full parsed data for debugging
            char* debug_json = cJSON_PrintUnformatted(parsed_data);
            log_info("[DingTalk] CALLBACK data content: %s", debug_json ? debug_json : "null");
            free(debug_json);
        }

        // Handle different message types
        const char* text = NULL;
        if (msg_type && cJSON_IsString(msg_type)) {
            if (strcmp(msg_type->valuestring, "text") == 0) {
                // text field is an object: {"content": "..."}
                if (text_content && cJSON_IsObject(text_content)) {
                    cJSON* content_str = cJSON_GetObjectItem(text_content, "content");
                    if (cJSON_IsString(content_str)) {
                        text = content_str->valuestring;
                    }
                }
            } else if (strcmp(msg_type->valuestring, "markdown") == 0) {
                // markdown field is an object: {"text": "..."}
                cJSON* markdown = cJSON_GetObjectItem(parsed_data, "markdown");
                if (markdown) {
                    cJSON* md_content = cJSON_GetObjectItem(markdown, "text");
                    if (cJSON_IsString(md_content)) {
                        text = md_content->valuestring;
                    }
                }
            }
        }

        // Send to callback
        if (conversation_id && cJSON_IsString(conversation_id) && text && ws->callback) {
            log_info("[DingTalk] Message from %s: %s",
                    sender_id ? sender_id->valuestring : "unknown", text);
            ws->callback(conversation_id->valuestring, text,
                        sender_id ? sender_id->valuestring : NULL,
                        session_webhook_str, ws->user_data);
        }

        // Send ACK for CALLBACK messages
        if (message_id && cJSON_IsString(message_id)) {
            cJSON* ack = cJSON_CreateObject();
            cJSON_AddNumberToObject(ack, "code", 200);
            cJSON_AddStringToObject(ack, "message", "ok");
            cJSON_AddStringToObject(ack, "data", "");
            cJSON* ack_headers = cJSON_CreateObject();
            cJSON_AddStringToObject(ack_headers, "contentType", "application/json");
            cJSON_AddStringToObject(ack_headers, "messageId", message_id->valuestring);
            cJSON_AddItemToObject(ack, "headers", ack_headers);
            char* ack_str = cJSON_PrintUnformatted(ack);
            if (ack_str && ws->c) {
                log_info("[DingTalk] Sending CALLBACK ACK: %s", ack_str);
                mg_ws_send(ws->c, ack_str, strlen(ack_str), WEBSOCKET_OP_TEXT);
                free(ack_str);
            }
            cJSON_Delete(ack);
        }

        cJSON_Delete(parsed_data);

    } else if (strcmp(type_str, "SYSTEM") == 0) {
        // SYSTEM: ping/pong, connection events
        headers = cJSON_GetObjectItem(root, "headers");
        if (headers) {
            cJSON* topic = cJSON_GetObjectItem(headers, "topic");
            if (topic && cJSON_IsString(topic)) {
                log_info("[DingTalk] SYSTEM message, topic: %s", topic->valuestring);
            }
        }

    } else if (strcmp(type_str, "EVENT") == 0) {
        // EVENT: other event notifications
        log_info("[DingTalk] EVENT message received");

    } else {
        log_info("[DingTalk] Unknown message type: %s", type_str);
    }

    cJSON_Delete(root);
}

// =============================================================================
// HMAC-SHA256 Signature Helper (Forward Declarations)
// =============================================================================

static int64_t get_timestamp_ms(void);
static char* compute_signature(const char* client_secret, int64_t timestamp_ms);

// =============================================================================
// WebSocket Communication
// =============================================================================

static void dingtalk_ws_send_frame(struct mg_connection* c, WSFrame* f) {
    ByteBuf b = {0};
    if (!encode_frame(f, &b)) {
        free(b.data);
        return;
    }
    mg_ws_send(c, (const char*)b.data, b.len, WEBSOCKET_OP_BINARY);
    free(b.data);
}

static void dingtalk_ws_send_ping(struct mg_connection* c, DingTalkWS* ws) {
    WSFrame f;
    memset(&f, 0, sizeof(f));
    f.method = 0;
    f.service = 1;  // Service ID for control
    frame_header_set(&f, "type", MSG_TYPE_PING);
    dingtalk_ws_send_frame(c, &f);
    frame_free(&f);
}

static void dingtalk_ws_send_ack(struct mg_connection* c, WSFrame* in, int code) {
    WSFrame out;
    cJSON* payload = cJSON_CreateObject();
    cJSON* headers = cJSON_CreateObject();
    char* pstr;
    size_t i;

    memset(&out, 0, sizeof(out));
    out.seq_id = in->seq_id;
    out.log_id = in->log_id;
    out.service = in->service;
    out.method = in->method;
    out.payload_type = in->payload_type ? strdup(in->payload_type) : NULL;

    // Copy headers
    for (i = 0; i < in->headers_count; i++) {
        if (in->headers[i].key && in->headers[i].value)
            frame_header_set(&out, in->headers[i].key, in->headers[i].value);
    }

    // Build response payload
    cJSON_AddNumberToObject(payload, "code", code);
    cJSON_AddItemToObject(payload, "headers", headers);
    cJSON_AddStringToObject(payload, "data", "");

    pstr = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);

    if (pstr) {
        out.payload = (unsigned char*)pstr;
        out.payload_len = strlen(pstr);
        dingtalk_ws_send_frame(c, &out);
        log_debug("[DingTalk] Sent ack: code=%d", code);
    }
    frame_free(&out);
}

// =============================================================================
// WebSocket Handler
// =============================================================================

static void ws_handler(struct mg_connection* c, int ev, void* ev_data) {
    DingTalkWS* ws = (DingTalkWS*)c->fn_data;

    // Log ALL events for debugging
    if (ev == MG_EV_OPEN) {
        log_info("[DingTalk] MG_EV_OPEN - Connection created");
        log_info("[DingTalk] Connection state: c=%p, fn_data=%p, is_client=%d",
                 (void*)c, (void*)c->fn_data, c->is_client);
        ws->next_ping_ms = now_ms() + ws->ping_interval_ms;
    } else if (ev == MG_EV_CONNECT) {
        log_info("[DingTalk] MG_EV_CONNECT - TCP connection established");
    } else if (ev == MG_EV_READ) {
        log_info("[DingTalk] MG_EV_READ - Data received from socket (%ld bytes)",
                 c->recv.len);
    } else if (ev == MG_EV_WS_OPEN) {
        // WebSocket handshake completed
        log_info("[DingTalk] MG_EV_WS_OPEN - WebSocket handshake completed (ready to receive messages)");
        log_info("[DingTalk] is_websocket=%d, c->pfn=%p, c->is_closing=%d", c->is_websocket, (void*)c->pfn, c->is_closing);
        ws->next_ping_ms = now_ms() + ws->ping_interval_ms;
    } else if (ev == MG_EV_CLOSE) {
        log_info("[DingTalk] MG_EV_CLOSE - Connection closing (is_websocket=%d, is_closing=%d)", c->is_websocket, c->is_closing);
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message* wm = (struct mg_ws_message*)ev_data;
        WSFrame f;
        char* payload_text = NULL;
        char* hex_dump = NULL;

        // Log ALL WebSocket messages with hex dump
        log_info("[DingTalk] === WebSocket message received ===");
        log_info("[DingTalk] Raw message length: %zu bytes", wm->data.len);

        // Hex dump first 256 bytes for debugging
        if (wm->data.len > 0) {
            size_t dump_len = wm->data.len < 256 ? wm->data.len : 256;
            hex_dump = malloc(dump_len * 3 + 1);
            if (hex_dump) {
                size_t j = 0;
                for (size_t i = 0; i < dump_len; i++) {
                    hex_dump[j++] = "0123456789abcdef"[(wm->data.buf[i] >> 4) & 0xF];
                    hex_dump[j++] = "0123456789abcdef"[wm->data.buf[i] & 0xF];
                    hex_dump[j++] = ' ';
                }
                hex_dump[j] = '\0';
                log_info("[DingTalk] Hex dump: %s", hex_dump);
                free(hex_dump);
            }
        }

        // Check if message is JSON format (starts with '{')
        // DingTalk sends SYSTEM messages as JSON, event messages as Protobuf
        if (wm->data.len > 0 && wm->data.buf[0] == '{') {
            // JSON format message (SYSTEM, PONG, etc.)
            char* json_str = NULL;
            cJSON* root = NULL;
            cJSON* type_item = NULL;
            const char* msg_type = NULL;

            json_str = (char*)malloc(wm->data.len + 1);
            if (json_str) {
                memcpy(json_str, wm->data.buf, wm->data.len);
                json_str[wm->data.len] = '\0';
            }

            log_info("[DingTalk] JSON message detected: %.100s...", json_str ? json_str : "");
            root = cJSON_Parse(json_str);
            if (root) {
                type_item = cJSON_GetObjectItem(root, "type");
                if (cJSON_IsString(type_item)) {
                    msg_type = type_item->valuestring;
                    log_info("[DingTalk] JSON message type: %s", msg_type);
                }

                // Handle SYSTEM message (connection confirmation)
                if (msg_type && strcmp(msg_type, "SYSTEM") == 0) {
                    // Print full SYSTEM message for debugging
                    log_info("[DingTalk] Full SYSTEM message: %s", json_str);

                    cJSON* headers = cJSON_GetObjectItem(root, "headers");
                    if (headers) {
                        // Print all headers for debugging
                        log_info("[DingTalk] SYSTEM message headers:");
                        cJSON* header_item = NULL;
                        cJSON* header_obj = headers;
                        if (cJSON_IsObject(header_obj)) {
                            cJSON_ArrayForEach(header_item, header_obj) {
                                if (cJSON_IsString(header_item)) {
                                    log_info("[DingTalk]   %s: %s", header_item->string, header_item->valuestring);
                                }
                            }
                        }

                        cJSON* ping_interval = cJSON_GetObjectItem(headers, "pingInterval");
                        if (cJSON_IsNumber(ping_interval)) {
                            ws->ping_interval_ms = (uint64_t)ping_interval->valueint * 1000;
                            log_info("[DingTalk] Ping interval from SYSTEM: %lums", (unsigned long)ws->ping_interval_ms);
                        }
                        // Extract messageId for ACK response
                        cJSON* msg_id = cJSON_GetObjectItem(headers, "messageId");
                        if (cJSON_IsString(msg_id)) {
                            // Send ACK response
                            cJSON* ack = cJSON_CreateObject();
                            cJSON_AddNumberToObject(ack, "code", 200);
                            cJSON_AddStringToObject(ack, "message", "ok");
                            cJSON_AddStringToObject(ack, "data", "");
                            cJSON* ack_headers = cJSON_CreateObject();
                            cJSON_AddStringToObject(ack_headers, "contentType", "application/json");
                            if (msg_id && msg_id->valuestring) {
                                cJSON_AddStringToObject(ack_headers, "messageId", msg_id->valuestring);
                            }
                            cJSON_AddItemToObject(ack, "headers", ack_headers);
                            char* ack_str = cJSON_PrintUnformatted(ack);
                            if (ack_str) {
                                log_info("[DingTalk] Sending SYSTEM ACK: %s", ack_str);
                                mg_ws_send(c, ack_str, strlen(ack_str), WEBSOCKET_OP_TEXT);
                                free(ack_str);
                            }
                            cJSON_Delete(ack);
                        }
                    }
                    log_info("[DingTalk] SYSTEM message received - connection established");
                }
                // Handle PONG message
                else if (msg_type && strcmp(msg_type, "PONG") == 0) {
                    cJSON* ping_interval = cJSON_GetObjectItem(root, "PingInterval");
                    if (cJSON_IsNumber(ping_interval)) {
                        ws->ping_interval_ms = (uint64_t)ping_interval->valueint * 1000;
                        log_info("[DingTalk] Ping interval from PONG: %lums", (unsigned long)ws->ping_interval_ms);
                    }
                    log_info("[DingTalk] PONG message received");
                }
                // Handle other JSON messages (might be events)
                else {
                    log_info("[DingTalk] Processing JSON message as potential event...");
                    handle_event_json(ws, json_str);
                }
                cJSON_Delete(root);
            } else {
                log_error("[DingTalk] Failed to parse JSON message");
            }
            free(json_str);
            return;
        }

        // Protobuf format message
        memset(&f, 0, sizeof(f));

        // Parse the Protobuf frame
        if (!parse_frame((const unsigned char*)wm->data.buf, wm->data.len, &f)) {
            log_error("[DingTalk] Failed to parse Protobuf frame");
            frame_free(&f);
            return;
        }

        log_info("[DingTalk] Parsed frame: seq=%lu, log_id=%lu, service=%d, method=%d, payload_type=%s",
                 (unsigned long)f.seq_id, (unsigned long)f.log_id, f.service, f.method,
                 f.payload_type ? f.payload_type : "null");

        // Handle control messages (method == 0)
        if (f.method == 0) {
            const char* msg_type = frame_header_get(&f, "type");
            log_info("[DingTalk] Control message, type=%s", msg_type ? msg_type : "null");

            if (msg_type && strcmp(msg_type, MSG_TYPE_PONG) == 0) {
                log_info("[DingTalk] Received pong response");
                // Parse pong response for ping interval
                if (f.payload && f.payload_len > 0) {
                    char* json_str = (char*)malloc(f.payload_len + 1);
                    if (json_str) {
                        memcpy(json_str, f.payload, f.payload_len);
                        json_str[f.payload_len] = '\0';
                        log_info("[DingTalk] Pong payload: %s", json_str);
                        cJSON* conf = cJSON_Parse(json_str);
                        if (conf) {
                            cJSON* pi = cJSON_GetObjectItem(conf, "PingInterval");
                            if (cJSON_IsNumber(pi) && pi->valueint > 0) {
                                ws->ping_interval_ms = (uint64_t)pi->valueint * 1000;
                                log_info("[DingTalk] Ping interval set to %lums", (unsigned long)ws->ping_interval_ms);
                            }
                            cJSON_Delete(conf);
                        }
                        free(json_str);
                    }
                }
            } else if (msg_type && strcmp(msg_type, MSG_TYPE_PING) == 0) {
                log_info("[DingTalk] Received ping (unexpected)");
            }
            frame_free(&f);
            return;
        }

        // Handle event messages (method == 1)
        if (f.method == 1) {
            const char* topic = frame_header_get(&f, "topic");
            const char* msg_id = frame_header_get(&f, "messageId");
            log_info("[DingTalk] Event message: topic=%s, messageId=%s, payload_len=%zu",
                     topic ? topic : "null", msg_id ? msg_id : "null", f.payload_len);

            if (f.payload && f.payload_len > 0) {
                payload_text = (char*)malloc(f.payload_len + 1);
                if (payload_text) {
                    memcpy(payload_text, f.payload, f.payload_len);
                    payload_text[f.payload_len] = '\0';
                    log_info("[DingTalk] Payload content: %s", payload_text);
                }
            } else {
                log_warn("[DingTalk] Event message has no payload");
            }

            if (payload_text) {
                log_info("[DingTalk] Parsing JSON event...");
                handle_event_json(ws, payload_text);
                free(payload_text);
            } else {
                log_error("[DingTalk] Failed to extract payload from event message");
            }

            // Send acknowledgment
            log_info("[DingTalk] Sending ACK");
            dingtalk_ws_send_ack(c, &f, 200);
        } else {
            log_info("[DingTalk] Unknown method type: %d", f.method);
        }

        frame_free(&f);
    } else if (ev == MG_EV_CONNECT) {
        log_info("[DingTalk] MG_EV_CONNECT - TCP connection established");
    } else if (ev == MG_EV_READ) {
        log_info("[DingTalk] MG_EV_READ - Data received from socket (%ld bytes)",
                 c->recv.len);
    } else if (ev == MG_EV_CLOSE) {
        log_info("[DingTalk] MG_EV_CLOSE - WebSocket connection closed");
        log_info("[DingTalk] Close state: c->is_closing=%d, ws->running=%d", c->is_closing, ws->running);
        ws->running = false;
    } else if (ev == MG_EV_ERROR) {
        log_error("[DingTalk] MG_EV_ERROR - %s", (char*)ev_data);
        log_error("[DingTalk] Error state: c->is_draining=%d, c->is_resp=%d", c->is_draining, c->is_resp);
        ws->running = false;
    } else if (ev == MG_EV_POLL) {
        // Skip poll event logs to reduce noise
    }
    // Skip logging unknown events to reduce noise
}

// =============================================================================
// Public API
// =============================================================================

DingTalkWS* dingtalk_ws_create(void) {
    DingTalkWS* ws = (DingTalkWS*)malloc(sizeof(DingTalkWS));
    if (!ws) return NULL;
    memset(ws, 0, sizeof(*ws));
    mg_mgr_init(&ws->mgr);
    ws->ping_interval_ms = 120000;  // Default 2 minutes
    ws->next_ping_ms = now_ms() + ws->ping_interval_ms;
    return ws;
}

void dingtalk_ws_destroy(DingTalkWS* ws) {
    int i;
    if (!ws) return;
    mg_mgr_free(&ws->mgr);
    for (i = 0; i < 20; i++)
        free(ws->recent_msg_ids[i]);
    free(ws->access_token);
    free(ws->ws_url);
    free(ws);
}

bool dingtalk_ws_connect(DingTalkWS* ws, const char* url, const char* access_token,
                         const char* client_secret) {
    (void)access_token;  // Access token not needed for WS connection (ticket in URL is used)
    (void)client_secret; // Client secret not needed for WS connection
    if (!ws || !url) return false;

    ws->access_token = access_token ? strdup(access_token) : NULL;
    ws->ws_url = strdup(url);

    // Connect using WebSocket
    // Note: DingTalk WebSocket uses ticket parameter in URL for authentication
    // Additional headers (x-acs-dingtalk-*) are not required for WebSocket upgrade
    ws->c = mg_ws_connect(&ws->mgr, url, ws_handler, ws, NULL);
    if (!ws->c) {
        log_error("[DingTalk] Connection failed");
        return false;
    }

    // Set up TLS if needed
    struct mg_str host = mg_url_host(url);
    struct mg_tls_opts opts = {0};
    opts.ca = mg_str("");
    opts.name = host;
    opts.skip_verification = true;
    if (mg_url_is_ssl(url)) {
        mg_tls_init(ws->c, &opts);
    }

    log_info("[DingTalk] Connected to %s", url);
    return true;
}

void dingtalk_ws_run(DingTalkWS* ws, DingTalkWSMessageCallback callback, void* user_data) {
    if (!ws) return;
    ws->callback = callback;
    ws->user_data = user_data;
    ws->running = true;

    log_info("[DingTalk] Starting WebSocket loop...");
    log_info("[DingTalk] Listening for messages (ping interval: %lums)", (unsigned long)ws->ping_interval_ms);

    while (ws->running && ws->mgr.conns) {
        mg_mgr_poll(&ws->mgr, 100);

        // Send periodic ping
        if (ws->c && now_ms() >= ws->next_ping_ms) {
            dingtalk_ws_send_ping(ws->c, ws);
            ws->next_ping_ms = now_ms() + ws->ping_interval_ms;
        }
    }

    log_info("[DingTalk] WebSocket loop ended");
}

void dingtalk_ws_stop(DingTalkWS* ws) {
    if (ws) {
        ws->running = false;
        if (ws->c) {
            ws->c->is_closing = 1;
        }
    }
}

// =============================================================================
// HMAC-SHA256 Signature Helper
// =============================================================================

// Convert binary data to hex string
static void bytes_to_hex(const unsigned char* bytes, size_t len, char* hex_str) {
    static const char hex_chars[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; i++) {
        hex_str[i * 2] = hex_chars[(bytes[i] >> 4) & 0xF];
        hex_str[i * 2 + 1] = hex_chars[bytes[i] & 0xF];
    }
    hex_str[len * 2] = '\0';
}

// Get current timestamp in milliseconds
static int64_t get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
}

// Compute HMAC-SHA256 signature for DingTalk API
// Signature = base64(HMAC-SHA256(client_secret, timestamp))
static char* compute_signature(const char* client_secret, int64_t timestamp_ms) {
    char ts_str[32];
    snprintf(ts_str, sizeof(ts_str), "%ld", (long)timestamp_ms);

    unsigned char digest[32];
    mg_hmac_sha256(digest,
                   (unsigned char*)client_secret, strlen(client_secret),
                   (unsigned char*)ts_str, strlen(ts_str));

    // Convert to hex string (DingTalk accepts hex-encoded signature)
    char* hex_sig = malloc(65);  // 32 bytes * 2 + 1
    if (hex_sig) {
        bytes_to_hex(digest, 32, hex_sig);
    }
    return hex_sig;
}

// Callback struct for WS URL request
typedef struct {
    char* memory;
    size_t size;
    bool done;
} URLChunk;

static void url_callback_fn(struct mg_connection* c, int ev, void* ev_data) {
    URLChunk* chunk = (URLChunk*)c->fn_data;

    // Log ALL events for debugging
    if (ev == MG_EV_OPEN) {
        log_info("[DingTalk] HTTP connection opened");
    } else if (ev == MG_EV_CONNECT) {
        log_info("[DingTalk] HTTP TCP connection established");
    } else if (ev == MG_EV_TLS_HS) {
        log_info("[DingTalk] TLS handshake completed");
    } else if (ev == MG_EV_READ) {
        log_info("[DingTalk] HTTP data received (%ld bytes)", c->recv.len);
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message* hm = (struct mg_http_message*)ev_data;
        log_info("[DingTalk] HTTP response received: %.*s %.*s",
                 (int)hm->method.len, hm->method.buf,
                 (int)hm->uri.len, hm->uri.buf);
        log_info("[DingTalk] HTTP body: %.*s", (int)hm->body.len, hm->body.buf);
        char* new_mem = realloc(chunk->memory, chunk->size + hm->body.len + 1);
        if (new_mem) {
            chunk->memory = new_mem;
            memcpy(chunk->memory + chunk->size, hm->body.buf, hm->body.len);
            chunk->size += hm->body.len;
            chunk->memory[chunk->size] = '\0';
        }
        chunk->done = true;
        // Don't close immediately - let mongoose handle it gracefully
    } else if (ev == MG_EV_ERROR) {
        log_error("[DingTalk] HTTP error fetching WS URL: %s", (char*)ev_data);
        chunk->done = true;
    } else if (ev == MG_EV_CLOSE) {
        log_info("[DingTalk] HTTP connection closed");
        // Connection closed, mark as done if not already
        chunk->done = true;
    }
}

char* dingtalk_get_ws_url(const char* client_id, const char* client_secret, const char* access_token) {
    struct mg_mgr mgr;
    URLChunk chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';

    mg_mgr_init(&mgr);

    // Configure DNS explicitly to avoid DNS timeout issues
    // Use Google DNS servers with longer timeout
    mgr.dns4.url = "udp://8.8.8.8:53";
    mgr.dns6.url = "udp://[2001:4860:4860::8888]:53";
    mgr.dnstimeout = 10000;  // 10 seconds timeout

    // DingTalk WebSocket connection API
    const char* url = "https://api.dingtalk.com/v1.0/gateway/connections/open";
    log_info("[DingTalk] Connecting to %s", url);

    struct mg_connection* c = mg_http_connect(&mgr, url, url_callback_fn, &chunk);
    log_info("[DingTalk] mg_http_connect returned: c=%p", (void*)c);

    if (!c) {
        log_error("[DingTalk] Failed to connect for WS URL");
        free(chunk.memory);
        mg_mgr_free(&mgr);
        return NULL;
    }

    // Set up TLS
    struct mg_str host = mg_url_host(url);
    struct mg_tls_opts opts = {0};
    opts.ca = mg_str("");
    opts.name = host;
    opts.skip_verification = true;
    if (mg_url_is_ssl(url)) {
        log_info("[DingTalk] Initializing TLS for host: %.*s", (int)host.len, host.buf);
        mg_tls_init(c, &opts);
    }

    // Get timestamp and compute signature
    int64_t timestamp_ms = get_timestamp_ms();
    char* signature = compute_signature(client_secret, timestamp_ms);
    char ts_str[32];
    snprintf(ts_str, sizeof(ts_str), "%ld", (long)timestamp_ms);

    // Build request body with client credentials and subscriptions
    char* json_str;
    {
        cJSON* payload = cJSON_CreateObject();
        // Send client_id and client_secret in the body
        if (client_id && client_secret) {
            cJSON_AddStringToObject(payload, "clientId", client_id);
            cJSON_AddStringToObject(payload, "clientSecret", client_secret);
        }

        // Add subscriptions - required to receive events
        // See: https://github.com/open-dingtalk/dingtalk-stream-sdk-go/blob/main/client/client.go#L354
        // Topics reference: https://github.com/open-dingtalk/dingtalk-stream-sdk-go/blob/main/payload/payload.go
        cJSON* subscriptions = cJSON_AddArrayToObject(payload, "subscriptions");

        // Subscribe to SYSTEM events (for connection management)
        // SYSTEM topics: "ping", "disconnect"
        cJSON* sys_sub = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_sub, "type", "SYSTEM");
        cJSON_AddStringToObject(sys_sub, "topic", "ping");
        cJSON_AddItemToArray(subscriptions, sys_sub);

        // Subscribe to EVENT events (for receiving chat messages)
        // EVENT topics: bot related events
        cJSON* event_sub = cJSON_CreateObject();
        cJSON_AddStringToObject(event_sub, "type", "EVENT");
        cJSON_AddStringToObject(event_sub, "topic", "*");
        cJSON_AddItemToArray(subscriptions, event_sub);

        // Subscribe to CALLBACK events (for bot messages - this is the main one for chat)
        // CALLBACK topics: "/v1.0/im/bot/messages/get" for bot messages
        cJSON* cb_sub = cJSON_CreateObject();
        cJSON_AddStringToObject(cb_sub, "type", "CALLBACK");
        cJSON_AddStringToObject(cb_sub, "topic", "/v1.0/im/bot/messages/get");
        cJSON_AddItemToArray(subscriptions, cb_sub);

        json_str = cJSON_PrintUnformatted(payload);
        cJSON_Delete(payload);
    }

    log_info("[DingTalk] Fetching WebSocket URL from %s (client_id: %s, timestamp: %ld)",
             url, client_id ? client_id : "null", (long)timestamp_ms);
    log_info("[DingTalk] Request body: %s", json_str);

    // Send HTTP request with HTTP/1.1 and proper headers including signature
    log_info("[DingTalk] Sending HTTP request...");
    mg_printf(c,
        "POST %s HTTP/1.0\r\n"
        "Host: %.*s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "x-acs-dingtalk-access-token: %s\r\n"
        "x-acs-dingtalk-signature: %s\r\n"
        "x-acs-dingtalk-timestamp: %s\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        mg_url_uri(url),
        (int)host.len, host.buf,
        access_token ? access_token : "",
        signature ? signature : "",
        ts_str,
        (int)strlen(json_str),
        json_str
    );

    log_info("[DingTalk] Starting HTTP poll loop...");
    while (!chunk.done) mg_mgr_poll(&mgr, 100);
    log_info("[DingTalk] HTTP poll loop ended");

    // Log raw response for debugging
    log_info("[DingTalk] WS URL response (%zu bytes): %.*s", chunk.size, (int)chunk.size, chunk.memory);
    log_info("[DingTalk] Parsing WS URL response...");

    char* ws_url = NULL;

    if (chunk.size > 0) {
        cJSON* resp = cJSON_Parse(chunk.memory);
        if (resp) {
            // Check for error response
            cJSON* code = cJSON_GetObjectItem(resp, "code");
            cJSON* message = cJSON_GetObjectItem(resp, "message");

            if (code && code->valueint != 0) {
                log_error("[DingTalk] Get WS URL failed: %d - %s",
                         code->valueint, message ? message->valuestring : "Unknown");
            } else {
                // Success - look for endpoint and ticket
                cJSON* endpoint = cJSON_GetObjectItem(resp, "endpoint");
                cJSON* ticket = cJSON_GetObjectItem(resp, "ticket");

                if (cJSON_IsString(endpoint) && cJSON_IsString(ticket)) {
                    // Build full URL: endpoint?ticket=xxx
                    size_t url_len = strlen(endpoint->valuestring) + strlen(ticket->valuestring) + 10;
                    ws_url = malloc(url_len);
                    if (ws_url) {
                        snprintf(ws_url, url_len, "%s?ticket=%s",
                                endpoint->valuestring, ticket->valuestring);
                        log_info("[DingTalk] Got WebSocket URL: %s", ws_url);
                    }
                } else {
                    // Try looking in data object
                    cJSON* data = cJSON_GetObjectItem(resp, "data");
                    if (data) {
                        endpoint = cJSON_GetObjectItem(data, "endpoint");
                        ticket = cJSON_GetObjectItem(data, "ticket");
                        if (cJSON_IsString(endpoint) && cJSON_IsString(ticket)) {
                            size_t url_len = strlen(endpoint->valuestring) + strlen(ticket->valuestring) + 10;
                            ws_url = malloc(url_len);
                            if (ws_url) {
                                snprintf(ws_url, url_len, "%s?ticket=%s",
                                        endpoint->valuestring, ticket->valuestring);
                                log_info("[DingTalk] Got WebSocket URL: %s", ws_url);
                            }
                        }
                    }
                }

                // If still no URL, try "url" at root level
                if (!ws_url) {
                    cJSON* url_item = cJSON_GetObjectItem(resp, "url");
                    if (cJSON_IsString(url_item)) {
                        ws_url = strdup(url_item->valuestring);
                        log_info("[DingTalk] Got WebSocket URL: %s", ws_url);
                    }
                }
            }
            cJSON_Delete(resp);
        } else {
            log_error("[DingTalk] Failed to parse WS URL response");
        }
    } else {
        log_error("[DingTalk] Empty response for WS URL");
    }

    free(json_str);
    free(signature);
    free(chunk.memory);
    mg_mgr_free(&mgr);

    return ws_url;
}
