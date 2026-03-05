#include "message.h"
#include "common.h"

Message* message_new(MessageRole role, const char* content) {
    Message* msg = malloc(sizeof(Message));
    if (!msg) return NULL;
    msg->role = role;
    msg->content = string_new(content);
    msg->timestamp = string_new(""); // Will be set later
    msg->tool_calls = NULL;
    msg->tool_calls_count = 0;
    msg->tool_call_id = string_new("");
    msg->name = string_new("");
    return msg;
}

void message_free(Message* msg) {
    if (!msg) return;
    string_free(&msg->content);
    string_free(&msg->timestamp);
    for (size_t i = 0; i < msg->tool_calls_count; i++) {
        string_free(&msg->tool_calls[i].id);
        string_free(&msg->tool_calls[i].name);
        string_free(&msg->tool_calls[i].arguments);
    }
    free(msg->tool_calls);
    string_free(&msg->tool_call_id);
    string_free(&msg->name);
    free(msg);
}

void message_add_tool_call(Message* msg, const char* id, const char* name, const char* args) {
    msg->tool_calls = realloc(msg->tool_calls, (msg->tool_calls_count + 1) * sizeof(ToolCall));
    msg->tool_calls[msg->tool_calls_count].id = string_new(id);
    msg->tool_calls[msg->tool_calls_count].name = string_new(name);
    msg->tool_calls[msg->tool_calls_count].arguments = string_new(args);
    msg->tool_calls_count++;
}

InboundMessage* inbound_message_new(const char* channel, const char* chat_id, const char* content) {
    InboundMessage* msg = malloc(sizeof(InboundMessage));
    if (!msg) return NULL;
    msg->channel = string_new(channel);
    msg->chat_id = string_new(chat_id);
    msg->content = string_new(content);
    msg->attachments = string_array_new();
    return msg;
}

void inbound_message_free(InboundMessage* msg) {
    if (!msg) return;
    string_free(&msg->channel);
    string_free(&msg->chat_id);
    string_free(&msg->content);
    string_array_free(&msg->attachments);
    free(msg);
}

OutboundMessage* outbound_message_new(const char* channel, const char* chat_id, const char* content) {
    OutboundMessage* msg = malloc(sizeof(OutboundMessage));
    if (!msg) return NULL;
    msg->channel = string_new(channel);
    msg->chat_id = string_new(chat_id);
    msg->content = string_new(content);
    msg->attachments = string_array_new();
    return msg;
}

void outbound_message_free(OutboundMessage* msg) {
    if (!msg) return;
    string_free(&msg->channel);
    string_free(&msg->chat_id);
    string_free(&msg->content);
    string_array_free(&msg->attachments);
    free(msg);
}