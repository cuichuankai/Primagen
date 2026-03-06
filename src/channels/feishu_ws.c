#include "feishu_ws.h"
#include "../include/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <unistd.h>
#include "../vendor/cJSON/cJSON.h"

struct FeishuWS {
    CURL* curl;
    bool running;
    char buffer[4096];
    size_t buffer_len;
    char* recent_msg_ids[20]; // Ring buffer for deduplication
    int msg_id_idx;
};

FeishuWS* feishu_ws_create() {
    FeishuWS* ws = malloc(sizeof(FeishuWS));
    ws->curl = NULL;
    ws->running = false;
    ws->buffer_len = 0;
    ws->msg_id_idx = 0;
    for (int i = 0; i < 20; i++) ws->recent_msg_ids[i] = NULL;
    return ws;
}

void feishu_ws_destroy(FeishuWS* ws) {
    if (ws) {
        if (ws->curl) curl_easy_cleanup(ws->curl);
        for (int i = 0; i < 20; i++) {
            if (ws->recent_msg_ids[i]) free(ws->recent_msg_ids[i]);
        }
        free(ws);
    }
}

static size_t ws_send(FeishuWS* ws, const void* data, size_t len) {
    size_t sent = 0;
    CURLcode res = curl_easy_send(ws->curl, data, len, &sent);
    if (res != CURLE_OK) {
        log_error("WS send failed: %s", curl_easy_strerror(res));
        return 0;
    }
    return sent;
}

static size_t ws_recv_timeout(FeishuWS* ws, void* buffer, size_t len, int timeout_ms) {
    size_t received = 0;
    while (timeout_ms > 0) {
        CURLcode res = curl_easy_recv(ws->curl, buffer, len, &received);
        if (res == CURLE_OK) return received;
        if (res == CURLE_AGAIN) {
            usleep(10000); // 10ms
            timeout_ms -= 10;
        } else {
            return 0;
        }
    }
    return 0;
}

// Simple Base64 encoder for the key (optional, can use fixed for now or implement)
// Fixed key: "dGhlIHNhbXBsZSBub25jZQ==" (from RFC 6455 example)

static void ws_init_buffer(FeishuWS* ws) {
    ws->buffer_len = 0;
    memset(ws->buffer, 0, sizeof(ws->buffer));
}

bool feishu_ws_connect(FeishuWS* ws, const char* url) {
    ws_init_buffer(ws);
    // URL format: wss://host/path...
    // We need to change wss:// to https:// for curl to handle SSL
    char https_url[1024];
    if (strncmp(url, "wss://", 6) == 0) {
        snprintf(https_url, sizeof(https_url), "https://%s", url + 6);
    } else {
        strncpy(https_url, url, sizeof(https_url) - 1);
    }

    ws->curl = curl_easy_init();
    if (!ws->curl) return false;

    curl_easy_setopt(ws->curl, CURLOPT_URL, https_url);
    curl_easy_setopt(ws->curl, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(ws->curl, CURLOPT_SSL_VERIFYPEER, 0L); // For testing
    curl_easy_setopt(ws->curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(ws->curl);
    if (res != CURLE_OK) {
        log_error("Connect failed: %s", curl_easy_strerror(res));
        return false;
    }

    // Perform Handshake
    // Extract Host and Path from URL
    char host[256] = {0};
    char path[1024] = {0};
    // skip https://
    char* p = strstr(https_url, "://");
    if (p) p += 3; else p = https_url;
    
    char* slash = strchr(p, '/');
    if (slash) {
        size_t host_len = slash - p;
        if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
        strncpy(host, p, host_len);
        strncpy(path, slash, sizeof(path) - 1);
    } else {
        strncpy(host, p, sizeof(host) - 1);
        strcpy(path, "/");
    }

    char req[2048];
    snprintf(req, sizeof(req), 
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, host);

    ws_send(ws, req, strlen(req));

    // Read response (simple)
    char resp[4096];
    size_t n = ws_recv_timeout(ws, resp, sizeof(resp) - 1, 5000); // 5s timeout
    if (n > 0) {
        resp[n] = 0;
        // printf("[Feishu] WS Handshake Response: %s\n", resp);
        if (strstr(resp, "101 Switching Protocols") || strstr(resp, "101")) {
            // Handshake success
            return true;
        }
    } else {
        // printf("[Feishu] WS Handshake No Response\n");
    }

    return false;
}

void feishu_ws_stop(FeishuWS* ws) {
    ws->running = false;
}

// Send PONG
static void send_pong(FeishuWS* ws, const char* data, size_t len) {
    unsigned char frame[16]; // Max header size
    frame[0] = 0x8A; // FIN + PONG
    
    size_t header_len = 2;
    if (len < 126) {
        frame[1] = 0x80 | len; // Masked
    } else {
        // Assume short PONG for now
        frame[1] = 0x80 | len;
    }
    
    // Masking key (4 bytes)
    unsigned char mask[4] = {0, 0, 0, 0}; // Null mask for simplicity
    
    // Write header
    // Combine frame parts to send in one go
    size_t total_len = header_len + 4 + len;
    unsigned char* buf = malloc(total_len);
    if (buf) {
        memcpy(buf, frame, header_len);
        memcpy(buf + header_len, mask, 4);
        if (len > 0) {
            memcpy(buf + header_len + 4, data, len);
        }
        ws_send(ws, buf, total_len);
        free(buf);
    }
}

// Minimal Protobuf Encoder for ACK
// We need to send an ACK with the message_id/trace_id we received.
// From observation/docs, we probably need to send back a protobuf message.
// Structure is likely mirroring the input but with type "ack"?
// Or just empty payload?

/*
   Go SDK:
   func (c *Client) sendAck(ctx context.Context, messageID string) error {
       return c.send(ctx, &Message{
           Headers: map[string]string{
               "message_id": messageID,
               "type":       "ack", // ? Not sure about key/value
           },
           PayloadType: "empty",
           Payload:     []byte{},
       })
   }
*/

// Helper to write varint
static void write_varint(unsigned char* buf, size_t* offset, uint64_t value) {
    while (value >= 0x80) {
        buf[(*offset)++] = (value & 0x7F) | 0x80;
        value >>= 7;
    }
    buf[(*offset)++] = value;
}

// Helper to write length-delimited field
static void write_bytes_field(unsigned char* buf, size_t* offset, int field_num, const char* data, size_t len) {
    uint64_t key = (field_num << 3) | 2; // WireType 2
    write_varint(buf, offset, key);
    write_varint(buf, offset, len);
    memcpy(buf + *offset, data, len);
    *offset += len;
}

// Minimal Map Encoder for Headers
// repeated FrameHeader headers = 1;
// message FrameHeader { string key = 1; string value = 2; }
/*
static void write_header_entry(unsigned char* buf, size_t* offset, const char* key, const char* value) {
    // size_t start = *offset;
    // We don't know length yet, reserve varint space (assume < 128 for total entry len)
    // Actually, headers are usually short.
    // Let's assume standard small headers.
    
    // Tag for Header Entry (Field 1, Wire 2)
    write_varint(buf, offset, (1 << 3) | 2);
    
    size_t len_offset = *offset;
    (*offset)++; // Reserve 1 byte for length (assuming < 128)
    
    size_t content_start = *offset;
    write_bytes_field(buf, offset, 1, key, strlen(key));
    write_bytes_field(buf, offset, 2, value, strlen(value));
    
    size_t content_len = *offset - content_start;
    buf[len_offset] = content_len; // Write length
}
*/

static void feishu_ws_send_ack(FeishuWS* ws, const char* message_id) {
    // Construct JSON Frame
    // Structure:
    // {
    //   "headers": {
    //     "message_id": "...",
    //     "type": "ack" // or "response"? Go SDK uses "type": "ack" in headers
    //   },
    //   "payload": { // If the user says "Payload.code=200", maybe payload is an object?
    //     "code": 200,
    //     "headers": {}, // Empty headers for the response payload
    //     "data": ""     // Empty data or object?
    //   }
    // }
    
    // However, Go SDK defines Frame with Payload as []byte.
    // If we are sending JSON, maybe "payload" field is just the data?
    // But the user said: "Return data... is structure: Frame{Headers[],Payload{code,headers[],data}}"
    // And "Payload.code=200".
    
    // Let's assume the Frame is a JSON object.
    
    cJSON* frame = cJSON_CreateObject();
    
    // 1. Headers
    cJSON* headers = cJSON_CreateObject();
    cJSON_AddStringToObject(headers, "message_id", message_id);
    cJSON_AddStringToObject(headers, "type", "ack");
    cJSON_AddNumberToObject(headers, "biz_rt", 10);
    // cJSON_AddStringToObject(headers, "status_code", "200"); // Maybe not needed if code is in payload?
    cJSON_AddItemToObject(frame, "headers", headers);
    
    // 2. Payload
    // The user said "Payload{code,headers[],data}".
    // This looks like a structured object, not raw bytes.
    // If the frame is JSON, payload can be an object.
    
    /*
      Wait, if Go SDK uses Protobuf, Frame.Payload is bytes.
      If I send JSON, Frame.payload might be expected to be a string?
      Or an object?
      
      If I assume the user is right about "Entire frame is JSON", then:
    */
    
    cJSON* payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "code", 200);
    cJSON_AddObjectToObject(payload, "headers");
    // cJSON_AddObjectToObject(payload, "data"); // Empty data
    // Or data should be empty string? "data": ""
    cJSON_AddStringToObject(payload, "data", "");
    
    // Now, do we add 'payload' as an object or as a JSON-stringified string?
    // Go SDK "Payload []byte" suggests it might be a string in JSON representation.
    // BUT the user described the structure "Payload{code...}" which implies structure.
    // Let's try adding it as an object first.
    // cJSON_AddItemToObject(frame, "payload", payload);
    
    // Correction: Go SDK Frame definition usually has "payload_type" too.
    // cJSON_AddStringToObject(frame, "payload_type", "json"); // ?
    
    // But let's stick to what the user explicitly said about structure:
    // Frame{Headers[], Payload{code,headers[],data}}
    // It seems Payload is a direct field.
    
    // Actually, if I look at how some JSON-RPC over WS works...
    // But Feishu might be special.
    
    // Let's try to stringify the payload object, because "Payload" in Frame is usually bytes/string.
    char* payload_str = cJSON_PrintUnformatted(payload);
    cJSON_AddStringToObject(frame, "payload", payload_str);
    cJSON_AddStringToObject(frame, "payload_type", "json"); // Guessing
    free(payload_str);
    
    // Wait, if I stringify it, it's a string.
    // If I add it as object, it's an object.
    // Given "Entire frame is JSON", maybe it's just a nested object.
    // cJSON_AddItemToObject(frame, "payload", payload); 
    
    // Let's try adding it as a JSON string for "payload" field, which is safer if the receiver expects bytes/string.
    // But the user said "Payload.code=200", implying we construct that structure.
    
    // Let's try the safest JSON approach:
    // Frame = { headers: {...}, payload: "{\"code\":200...}" }
    // This matches the "Frame" having a "Payload" field which contains the data.
    
    // But maybe the user means the Frame IS the Payload?
    // No, "Frame{Headers...}"
    
    // Let's go with: Frame is JSON object, Payload is a JSON string inside it.
    
    // Re-creating payload to match exact user desc: Payload{code,headers[],data}
    cJSON_Delete(payload); // clear previous
    payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "code", 200);
    cJSON_AddObjectToObject(payload, "headers");
    cJSON_AddStringToObject(payload, "data", "");
    
    char* inner_payload = cJSON_PrintUnformatted(payload);
    cJSON_AddStringToObject(frame, "payload", inner_payload);
    free(inner_payload);
    cJSON_Delete(payload);
    
    char* frame_json = cJSON_PrintUnformatted(frame);
    cJSON_Delete(frame);
    
    size_t len = strlen(frame_json);
    
    // Send TEXT Frame (0x81)
    unsigned char frame_head[10];
    frame_head[0] = 0x82; // FIN + TEXT
    size_t header_len = 2;
    if (len < 126) {
        frame_head[1] = 0x80 | len; // Masked
    } else if (len < 65536) {
        frame_head[1] = 0x80 | 126;
        frame_head[2] = (len >> 8) & 0xFF;
        frame_head[3] = len & 0xFF;
        header_len = 4;
    }
    
    unsigned char mask[4] = {0, 0, 0, 0};
    
    ws_send(ws, frame_head, header_len);
    ws_send(ws, mask, 4);
    ws_send(ws, frame_json, len); // Masked with 0
    
    free(frame_json);
}

// Helper to read Varint
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

// Helper to skip field
static void skip_field(const unsigned char* data, size_t len, size_t* offset, int wire_type) {
    if (wire_type == 0) { // Varint
        read_varint(data, len, offset);
    } else if (wire_type == 2) { // Length Delimited
        uint64_t l = read_varint(data, len, offset);
        *offset += l;
    } else if (wire_type == 1) { // 64-bit
        *offset += 8;
    } else if (wire_type == 5) { // 32-bit
        *offset += 4;
    }
}

/*
// Simple Proto Parser to extract specific string field (tag)
static char* get_proto_string(const unsigned char* data, size_t len, int tag) {
    // ...
}
*/

/* 
static FeishuFrame parse_feishu_frame(const unsigned char* data, size_t len) {
    // ...
}
*/

void feishu_ws_run(FeishuWS* ws, FeishuWSMessageCallback callback, void* user_data) {
    ws->running = true;
    unsigned char buf[4096];
    
    while (ws->running) {
        size_t n = ws_recv_timeout(ws, buf, sizeof(buf), 100);
        if (n == 0) {
            // usleep(100000); // Handled by recv_timeout
            continue;
        }
        
        // printf("[Feishu] WS Received %zu bytes\n", n); // Debug
        // printf("[Feishu] WS Received %zu bytes\n", n); // Debug
        
        // Simple parser
        size_t offset = 0;
        while (offset < n) {
            // Check if enough data for header
            if (n - offset < 2) break; // Incomplete header

            unsigned char b0 = buf[offset];
            unsigned char b1 = buf[offset + 1];
            
            // bool fin = (b0 & 0x80) != 0;
            int opcode = b0 & 0x0F;
            bool masked = (b1 & 0x80) != 0;
            uint64_t payload_len = b1 & 0x7F;

            // printf("[Feishu] WS Frame: opcode=%x len=%llu masked=%d\n", opcode, payload_len, masked); // Debug
            // printf("[Feishu] WS Frame: opcode=%x len=%llu masked=%d\n", opcode, payload_len, masked); // Debug
            
            size_t header_size = 2;
            if (payload_len == 126) {
                if (n - offset < 4) break;
                payload_len = (buf[offset+2] << 8) | buf[offset+3];
                header_size = 4;
            } else if (payload_len == 127) {
                if (n - offset < 10) break;
                // Simplified: ignore high bytes
                payload_len = ((uint64_t)buf[offset+8] << 8) | buf[offset+9]; // Very rough
                header_size = 10;
            }

            if (masked) header_size += 4;

            if (n - offset < header_size + payload_len) {
                // Incomplete frame, wait for more?
                // For simplicity, we assume small frames fit in buffer
                // If not, we drop it or implement a ring buffer (TODO)
                break; 
            }

            unsigned char* payload = buf + offset + header_size;
            
            if (masked) {
                // Unmask
                unsigned char mask[4];
                memcpy(mask, buf + offset + header_size - 4, 4);
                for (size_t i = 0; i < payload_len; i++) {
                    payload[i] ^= mask[i % 4];
                }
            }

            if (opcode == 0x9) { // PING
                send_pong(ws, (char*)payload, payload_len);
            } else if (opcode == 0x1 || opcode == 0x2) { // TEXT or BINARY
                // Attempt to decode Protobuf Frame
                // Feishu WS Frame (Top Level):
                // 1: headers (map)
                // 2: payload_type (string)
                // 3: payload (bytes) - This is the actual JSON event
                
                size_t pb_offset = 0;
                char* event_json = NULL;
                char* message_id = NULL; // Extract message_id for ACK

                // Simple manual parsing since we only need field 3 (payload)
                // Also extracting message_id from headers (Field 1)
                // Field 1 is repeated FrameHeader (key=1, value=2)
                
                while (pb_offset < payload_len) {
                    uint64_t key = read_varint(payload, payload_len, &pb_offset);
                    int field_num = key >> 3;
                    int wire_type = key & 0x7;
                    
                    if (field_num == 1 && wire_type == 2) { // Headers
                        uint64_t len = read_varint(payload, payload_len, &pb_offset);
                        size_t end = pb_offset + len;
                        if (end <= payload_len) {
                            // Parse Header Entry: key=1, value=2
                            size_t h_off = pb_offset;
                            char* h_key = NULL;
                            char* h_val = NULL;
                            
                            while (h_off < end) {
                                uint64_t h_key_int = read_varint(payload, end, &h_off);
                                int h_field = h_key_int >> 3;
                                int h_wire = h_key_int & 0x7;
                                if (h_wire == 2) {
                                    uint64_t h_len = read_varint(payload, end, &h_off);
                                    if (h_off + h_len <= end) {
                                        char* str = malloc(h_len + 1);
                                        memcpy(str, payload + h_off, h_len);
                                        str[h_len] = 0;
                                        if (h_field == 1) h_key = str;
                                        else if (h_field == 2) h_val = str;
                                        else free(str);
                                        h_off += h_len;
                                    }
                                } else {
                                    skip_field(payload, end, &h_off, h_wire);
                                }
                            }
                            
                            if (h_key && h_val) {
                                if (strcmp(h_key, "message_id") == 0) {
                                    if (message_id) free(message_id);
                                    message_id = strdup(h_val);
                                }
                            }
                            if (h_key) free(h_key);
                            if (h_val) free(h_val);
                            
                            pb_offset = end;
                        }
                    } else if (field_num == 3 && wire_type == 2) {
                        uint64_t data_len = read_varint(payload, payload_len, &pb_offset);
                        
                        if (pb_offset + data_len <= payload_len) {
                            // If we already found a payload, maybe free it? 
                            // But usually there's only one main payload.
                            if (event_json) free(event_json);
                            
                            event_json = malloc(data_len + 1);
                            memcpy(event_json, payload + pb_offset, data_len);
                            event_json[data_len] = 0;
                            pb_offset += data_len;
                        }
                    } else if (field_num == 8 && wire_type == 2) {
                        uint64_t data_len = read_varint(payload, payload_len, &pb_offset);
                        if (pb_offset + data_len <= payload_len) {
                             // This might also be payload in some versions?
                             // Let's treat it same as Field 3 if Field 3 wasn't found
                             if (!event_json) {
                                event_json = malloc(data_len + 1);
                                memcpy(event_json, payload + pb_offset, data_len);
                                event_json[data_len] = 0;
                             }
                             pb_offset += data_len;
                        }
                    } else {
                        skip_field(payload, payload_len, &pb_offset, wire_type);
                    }
                }

                // Send ACK if we found message_id
                if (message_id) {
                    feishu_ws_send_ack(ws, message_id);
                    free(message_id);
                    message_id = NULL; // Prevent double free or reuse
                }

                if (event_json) {
                    cJSON* root = cJSON_Parse(event_json);
                    if (root) {
                        // ...
                        cJSON* header = cJSON_GetObjectItem(root, "header");
                        // Also check for "type" at root level which some events use
                        cJSON* type = cJSON_GetObjectItem(root, "type");
                        
                        if (header) {
                            cJSON* event_type = cJSON_GetObjectItem(header, "event_type");
                            cJSON* event_id = cJSON_GetObjectItem(header, "event_id"); // Use event_id for deduplication
                            
                            if (event_id && event_id->valuestring) {
                                // Deduplication check using event_id
                                bool duplicate = false;
                                for (int i = 0; i < 20; i++) {
                                    if (ws->recent_msg_ids[i] && strcmp(ws->recent_msg_ids[i], event_id->valuestring) == 0) {
                                        duplicate = true;
                                        break;
                                    }
                                }
                                
                                if (!duplicate) {
                                    if (ws->recent_msg_ids[ws->msg_id_idx]) free(ws->recent_msg_ids[ws->msg_id_idx]);
                                    ws->recent_msg_ids[ws->msg_id_idx] = strdup(event_id->valuestring);
                                    ws->msg_id_idx = (ws->msg_id_idx + 1) % 20;
                                } else {
                                    // It's a duplicate!
                                    cJSON_Delete(root);
                                    free(event_json);
                                    goto next_frame;
                                }
                            }

                            // Check for message event v1
                            if (event_type && strcmp(event_type->valuestring, "im.message.receive_v1") == 0) {
                                // ... (Process V2 Event)
                                cJSON* event = cJSON_GetObjectItem(root, "event");
                                if (event) {
                                    cJSON* message = cJSON_GetObjectItem(event, "message");
                                    cJSON* sender = cJSON_GetObjectItem(event, "sender");
                                    
                                    if (message && sender) {
                                        cJSON* chat_id = cJSON_GetObjectItem(message, "chat_id");
                                        cJSON* content = cJSON_GetObjectItem(message, "content");
                                        cJSON* sender_id_obj = cJSON_GetObjectItem(sender, "sender_id");
                                        cJSON* open_id = sender_id_obj ? cJSON_GetObjectItem(sender_id_obj, "open_id") : NULL;
                                        
                                        if (chat_id && content && open_id) {
                                            char* text = NULL;
                                            if (cJSON_IsString(content) && content->valuestring) {
                                                cJSON* inner = cJSON_Parse(content->valuestring);
                                                if (inner) {
                                                    cJSON* txt = cJSON_GetObjectItem(inner, "text");
                                                    if (txt && cJSON_IsString(txt) && txt->valuestring) {
                                                        text = strdup(txt->valuestring);
                                                    }
                                                    cJSON_Delete(inner);
                                                }
                                            }

                                            if (text) {
                                                callback(chat_id->valuestring, text, open_id->valuestring, user_data);
                                                free(text);
                                            }
                                        }
                                    }
                                }
                            }
                        } 
                        
                        // Fallback: Check for V1 style or other structures if header is missing or different
                        // Some events might just have "uuid" and "payload" or similar
                        if (type && strcmp(type->valuestring, "event") == 0) {
                             // This might be the outer wrapper we saw in logs:
                             // {"type":"event", "challenge":..., "event":{...}}
                             // Check if there is an inner event
                             cJSON* event = cJSON_GetObjectItem(root, "event");
                             if (event) {
                                 // Check if it's a message event inside
                                 if (cJSON_HasObjectItem(event, "message")) {
                                     // It's likely a message!
                                    cJSON* message = cJSON_GetObjectItem(event, "message");
                                    // ... (Same extraction logic)
                                    if (message) {
                                        // Try to extract content directly from message object
                                        // (Simplified logic duplication for robustness)
                                         // cJSON* chat_id = cJSON_GetObjectItem(message, "chat_id");
                                         // ...
                                         // Actually, let's just reuse the logic above by jumping or refactoring?
                                         // For now, let's copy the extraction block to be safe
                                         cJSON* content = cJSON_GetObjectItem(message, "content");
                                          // Sender might be at root or inside event?
                                          // V1/V2 differences are tricky.
                                          // Let's assume V2 structure inside "event"
                                          cJSON* sender = cJSON_GetObjectItem(event, "sender");
                                           if (message && sender) {
                                                cJSON* chat_id = cJSON_GetObjectItem(message, "chat_id");
                                                cJSON* sender_id_obj = cJSON_GetObjectItem(sender, "sender_id");
                                                cJSON* open_id = sender_id_obj ? cJSON_GetObjectItem(sender_id_obj, "open_id") : NULL;
                                                
                                                if (chat_id && content && open_id) {
                                                    char* text = NULL;
                                                    // Content might be object or string
                                                    if (cJSON_IsString(content)) {
                                                        cJSON* inner = cJSON_Parse(content->valuestring);
                                                        if (inner) {
                                                            cJSON* txt = cJSON_GetObjectItem(inner, "text");
                                                            if (txt) text = strdup(txt->valuestring);
                                                            cJSON_Delete(inner);
                                                        }
                                                    } else if (cJSON_IsObject(content)) {
                                                        cJSON* txt = cJSON_GetObjectItem(content, "text");
                                                        if (txt) text = strdup(txt->valuestring);
                                                    }

                                                    if (text) {
                                                        callback(chat_id->valuestring, text, open_id->valuestring, user_data);
                                                        free(text);
                                                    }
                                                }
                                           }
                                     }
                                 }
                             }
                        }

                        cJSON_Delete(root);
                    }
                    free(event_json);
                } else {
                    // Try direct JSON parse as fallback (heuristic)
                    char* json_str = malloc(payload_len + 1);
                    memcpy(json_str, payload, payload_len);
                    json_str[payload_len] = 0;
                    
                    char* json_start = strchr(json_str, '{');
                    if (json_start) {
                        cJSON* root = cJSON_Parse(json_start);
                        if (root) {
                            // Reuse logic? Or just skip for now as this path is unlikely for binary frames
                            cJSON_Delete(root);
                        }
                    }
                    free(json_str);
                }

            } else if (opcode == 0x8) { // CLOSE
                ws->running = false;
            }

        next_frame:
            offset += header_size + payload_len;
        }
    }
}
