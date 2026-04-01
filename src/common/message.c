#include "message.h"
#include "common.h"
#include <stdlib.h>
#include <string.h>

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
    if (msg->tool_calls) {
        for (size_t i = 0; i < msg->tool_calls_count; i++) {
            string_free(&msg->tool_calls[i].id);
            string_free(&msg->tool_calls[i].name);
            string_free(&msg->tool_calls[i].arguments);
        }
        free(msg->tool_calls);
    }
    string_free(&msg->tool_call_id);
    string_free(&msg->name);
    free(msg);
}

InternalEvent* internal_event_new_llm_result(const char* session_key, Error err, const char* response, ToolCall* calls, size_t count) {
    InternalEvent* ev = calloc(1, sizeof(InternalEvent));
    if (!ev) return NULL;
    ev->type = EVENT_LLM_RESULT;
    ev->session_key = string_new(session_key ? session_key : "");
    ev->llm_error = err;
    ev->llm_response = string_new(response ? response : "");
    if (count > 0 && calls) {
        ev->tool_calls = calloc(count, sizeof(ToolCall));
        for (size_t i = 0; i < count; i++) {
            ev->tool_calls[i].id = string_copy(&calls[i].id);
            ev->tool_calls[i].name = string_copy(&calls[i].name);
            ev->tool_calls[i].arguments = string_copy(&calls[i].arguments);
        }
        ev->tool_calls_count = count;
    }
    return ev;
}

InternalEvent* internal_event_new_tool_result(const char* session_key, const char* tool_call_id, const char* tool_name, const char* result, Error err) {
    InternalEvent* ev = calloc(1, sizeof(InternalEvent));
    if (!ev) return NULL;
    ev->type = EVENT_TOOL_RESULT;
    ev->session_key = string_new(session_key ? session_key : "");
    ev->tool_call_id = string_new(tool_call_id ? tool_call_id : "");
    ev->tool_name = string_new(tool_name ? tool_name : "");
    ev->tool_result = string_new(result ? result : "");
    ev->tool_error = err;
    return ev;
}

void internal_event_free(InternalEvent* ev) {
    if (!ev) return;
    string_free(&ev->session_key);
    if (ev->type == EVENT_LLM_RESULT) {
        string_free(&ev->llm_response);
        for (size_t i = 0; i < ev->tool_calls_count; i++) {
            string_free(&ev->tool_calls[i].id);
            string_free(&ev->tool_calls[i].name);
            string_free(&ev->tool_calls[i].arguments);
        }
        free(ev->tool_calls);
    } else if (ev->type == EVENT_TOOL_RESULT) {
        string_free(&ev->tool_call_id);
        string_free(&ev->tool_name);
        string_free(&ev->tool_result);
    }
    free(ev);
}

void message_add_tool_call(Message* msg, const char* id, const char* name, const char* args) {
    ToolCall* new_calls = realloc(msg->tool_calls, (msg->tool_calls_count + 1) * sizeof(ToolCall));
    if (!new_calls) return;
    msg->tool_calls = new_calls;
    
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
