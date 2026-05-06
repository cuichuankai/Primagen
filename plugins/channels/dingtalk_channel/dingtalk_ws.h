#ifndef DINGTALK_WS_H
#define DINGTALK_WS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward declaration
struct mg_mgr;
struct mg_connection;

// DingTalk WebSocket message callback
// session_webhook: URL for replying to the conversation (use POST with {"msgtype":"markdown","markdown":{"content":"..."}})
typedef void (*DingTalkWSMessageCallback)(const char* conversation_id,
                                           const char* content,
                                           const char* sender_id,
                                           const char* sender_name,
                                           const char* session_webhook,
                                           int64_t webhook_expired_time,
                                           void* user_data);

// Opaque handle
typedef struct DingTalkWS DingTalkWS;

// Create WebSocket client
DingTalkWS* dingtalk_ws_create(void);

// Destroy WebSocket client
void dingtalk_ws_destroy(DingTalkWS* ws);
void dingtalk_ws_set_dns(DingTalkWS* ws, const char* dns4, const char* dns6, int dns_timeout_ms, bool use_system_resolver);

// Connect to WebSocket server (url format: wss://...?ticket=xxx)
// Requires access_token and client_secret for authentication headers
bool dingtalk_ws_connect(DingTalkWS* ws, const char* url, const char* access_token,
                         const char* client_secret);

// Run event loop (blocking)
void dingtalk_ws_run(DingTalkWS* ws, DingTalkWSMessageCallback callback, void* user_data);

// Stop the event loop
void dingtalk_ws_stop(DingTalkWS* ws);

bool dingtalk_ws_can_send_ping(const struct mg_connection* c);

// Get WebSocket connection URL from DingTalk API
char* dingtalk_get_ws_url(const char* client_id, const char* client_secret, const char* access_token,
                          const char* dns4, const char* dns6, int dns_timeout_ms, bool use_system_resolver);

#endif // DINGTALK_WS_H
