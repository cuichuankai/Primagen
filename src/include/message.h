#ifndef MESSAGE_H
#define MESSAGE_H

#include "common.h"

typedef enum {
    ROLE_USER,
    ROLE_ASSISTANT,
    ROLE_TOOL
} MessageRole;

typedef struct {
    String id;
    String name;
    String arguments; // JSON string
} ToolCall;

typedef struct {
    MessageRole role;
    String content;
    String timestamp; // ISO format
    ToolCall* tool_calls; // For assistant
    size_t tool_calls_count;
    String tool_call_id; // For tool
    String name; // For tool
} Message;

typedef struct {
    String channel;
    String chat_id;
    String content;
    StringArray attachments;
} InboundMessage;

typedef struct {
    String channel;
    String chat_id;
    String content;
    StringArray attachments;
} OutboundMessage;

// Functions
Message* message_new(MessageRole role, const char* content);
void message_free(Message* msg);
void message_add_tool_call(Message* msg, const char* id, const char* name, const char* args);
InboundMessage* inbound_message_new(const char* channel, const char* chat_id, const char* content);
void inbound_message_free(InboundMessage* msg);
OutboundMessage* outbound_message_new(const char* channel, const char* chat_id, const char* content);
void outbound_message_free(OutboundMessage* msg);

#endif // MESSAGE_H