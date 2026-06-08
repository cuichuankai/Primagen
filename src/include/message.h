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
    String sender_name;
    StringArray attachments;
    bool no_session_record;
} InboundMessage;

typedef struct {
    String channel;
    String chat_id;
    String content;
    StringArray attachments;
} OutboundMessage;

// Internal Event types for AgentLoop State Machine
typedef enum {
    EVENT_LLM_RESULT,
    EVENT_TOOL_RESULT
} InternalEventType;

typedef struct {
    InternalEventType type;
    String session_key;
    
    // For EVENT_LLM_RESULT
    Error llm_error;
    String llm_response;
    ToolCall* tool_calls;
    size_t tool_calls_count;
    
    // For EVENT_TOOL_RESULT
    String tool_call_id;
    String tool_name;
    String tool_result;
    Error tool_error;
} InternalEvent;

// Functions
Message* message_new(MessageRole role, const char* content);
void message_free(Message* msg);
void message_add_tool_call(Message* msg, const char* id, const char* name, const char* args);
InboundMessage* inbound_message_new(const char* channel, const char* chat_id, const char* content, const char* sender_name);
void inbound_message_free(InboundMessage* msg);
OutboundMessage* outbound_message_new(const char* channel, const char* chat_id, const char* content);
void outbound_message_free(OutboundMessage* msg);

InternalEvent* internal_event_new_llm_result(const char* session_key, Error err, const char* response, ToolCall* calls, size_t count);
InternalEvent* internal_event_new_tool_result(const char* session_key, const char* tool_call_id, const char* tool_name, const char* result, Error err);
void internal_event_free(InternalEvent* event);

#endif // MESSAGE_H