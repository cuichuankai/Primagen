#ifndef FEISHU_WS_H
#define FEISHU_WS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct FeishuWS FeishuWS;

// Callback for received text messages
typedef void (*FeishuWSMessageCallback)(const char* chat_id, const char* content, const char* sender_id, void* user_data);

FeishuWS* feishu_ws_create();
void feishu_ws_destroy(FeishuWS* ws);

// Connect to the WebSocket URL
bool feishu_ws_connect(FeishuWS* ws, const char* url);

// Run the main loop (blocking)
void feishu_ws_run(FeishuWS* ws, FeishuWSMessageCallback callback, void* user_data);

// Stop the loop
void feishu_ws_stop(FeishuWS* ws);

#endif // FEISHU_WS_H
