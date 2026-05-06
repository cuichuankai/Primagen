#ifndef COMMAND_DISPATCHER_H
#define COMMAND_DISPATCHER_H

#include <stddef.h>
#include <stdbool.h>
#include "../include/plugin.h"
#include "../include/message.h"
#include "../session/session.h"

struct AgentLoop;

typedef struct {
    struct AgentLoop* loop;
} CommandDispatcher;

CommandDispatcher* command_dispatcher_new(struct AgentLoop* loop);
void command_dispatcher_free(CommandDispatcher* dispatcher);

void command_dispatcher_register_builtin_commands(CommandDispatcher* dispatcher);

bool command_dispatcher_handle_message(CommandDispatcher* dispatcher, InboundMessage* inbound, Session* session, const char* session_key, const char* content);

CommandContext* command_context_new(struct AgentLoop* loop, const char* channel, const char* chat_id, const char* session_key);
void command_context_free(CommandContext* ctx);

#endif // COMMAND_DISPATCHER_H
