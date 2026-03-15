#include "agent_loop.h"
#include "../include/common.h"
#include "../include/logger.h"
#include "../tools/tools_impl.h"
#include "../include/utils.h"
#include <string.h>
#include <stdio.h>

/* Forward declaration - defined in utils.c */
extern char* strip_think_tags(const char* text);

/* Create a new AgentLoop instance */
AgentLoop* agent_loop_new(SessionManager* session_mgr, ContextBuilder* ctx_builder, ToolRegistry* tool_reg, MessageBus* bus, Config* config) {
    AgentLoop* loop = malloc(sizeof(AgentLoop));
    if (!loop) return NULL;

    loop->session_mgr = session_mgr;
    loop->ctx_builder = ctx_builder;
    loop->tool_reg = tool_reg;
    loop->bus = bus;
    loop->config = config;
    loop->running = false;
    loop->llm_call = NULL;
    loop->active_tasks = NULL;
    loop->current_session_key[0] = '\0';
    pthread_mutex_init(&loop->task_mutex, NULL);

    log_debug("[AgentLoop] Created new instance");
    return loop;
}

/* Free AgentLoop resources */
void agent_loop_free(AgentLoop* loop) {
    if (!loop) return;

    /* Free active tasks */
    pthread_mutex_lock(&loop->task_mutex);
    ActiveTaskNode* current = loop->active_tasks;
    while (current) {
        ActiveTaskNode* next = current->next;
        free(current);
        current = next;
    }
    loop->active_tasks = NULL;
    pthread_mutex_unlock(&loop->task_mutex);

    pthread_mutex_destroy(&loop->task_mutex);
    log_debug("[AgentLoop] Freed instance");
    free(loop);
}

/* Set the LLM provider function pointer */
void agent_loop_set_llm_provider(AgentLoop* loop, LLMProvider provider) {
    if (!loop) return;
    loop->llm_call = provider;
    log_debug("[AgentLoop] LLM provider set");
}

/* Stop the agent loop */
void agent_loop_stop(AgentLoop* loop) {
    if (!loop) return;
    loop->running = false;
    log_debug("[AgentLoop] Stop requested");
}

/* Helper: Clear dynamic array contents */
static void dynamic_array_clear_impl(DynamicArray* arr) {
    if (!arr) return;
    arr->count = 0;
}

/* Helper: Send error response to user without polluting session */
static void send_error_response(AgentLoop* loop, const char* channel, const char* chat_id, const char* error_msg) {
    char full_msg[512];
    snprintf(full_msg, sizeof(full_msg), "Sorry, I encountered an error: %s", error_msg);
    OutboundMessage* outbound = outbound_message_new(channel, chat_id, full_msg);
    message_bus_send_outbound(loop->bus, outbound);
}

/* Handle /stop command - cancel active tasks for a session */
static int handle_stop(AgentLoop* loop, const char* session_key, const char* channel, const char* chat_id) {
    if (!loop || !session_key) return 0;

    int cancelled = 0;
    pthread_mutex_lock(&loop->task_mutex);

    ActiveTaskNode* current = loop->active_tasks;
    while (current) {
        if (strcmp(current->session_key, session_key) == 0 && !current->cancelling) {
            current->cancelling = true;
            /* Note: In a full implementation, we'd signal the thread to stop */
            cancelled++;
        }
        current = current->next;
    }

    pthread_mutex_unlock(&loop->task_mutex);

    char response[256];
    if (cancelled > 0) {
        snprintf(response, sizeof(response), "Stopped %d task(s).", cancelled);
    } else {
        snprintf(response, sizeof(response), "No active task to stop.");
    }

    OutboundMessage* outbound = outbound_message_new(channel, chat_id, response);
    message_bus_send_outbound(loop->bus, outbound);

    log_debug("[AgentLoop] /stop command handled, cancelled=%d", cancelled);
    return cancelled;
}

/* Handle /restart command - placeholder for external wrapper */
static void handle_restart(AgentLoop* loop, const char* channel, const char* chat_id) {
    if (!loop) return;

    OutboundMessage* outbound = outbound_message_new(channel, chat_id, "Restarting...");
    message_bus_send_outbound(loop->bus, outbound);

    /* Note: Full restart would require process-level restart */
    /* This is a placeholder - actual restart needs shell wrapper */
    log_info("[AgentLoop] Restart requested but requires external wrapper");
}

/* Handle /new command - clear session and start fresh */
static void handle_new(AgentLoop* loop, const char* channel, const char* chat_id, Session* session) {
    if (!loop || !session) return;

    /* Clear session messages */
    for (size_t i = 0; i < session->messages.count; i++) {
        Message* msg = *(Message**)dynamic_array_get(&session->messages, i);
        message_free(msg);
    }
    dynamic_array_clear_impl(&session->messages);
    session->last_consolidated = 0;
    session->updated_at = time(NULL);

    /* Save cleared session */
    session_manager_save(loop->session_mgr, session);

    OutboundMessage* outbound = outbound_message_new(channel, chat_id, "New session started.");
    message_bus_send_outbound(loop->bus, outbound);

    log_debug("[AgentLoop] /new command handled - session cleared");
}

/* Handle /help command - show available commands */
static void handle_help(const char* channel, const char* chat_id, MessageBus* bus) {
    const char* help_text =
        "Primagen commands:\n"
        "/new — Start a new conversation\n"
        "/stop — Stop the current task\n"
        "/restart — Restart the bot\n"
        "/help — Show available commands";

    OutboundMessage* outbound = outbound_message_new(channel, chat_id, help_text);
    message_bus_send_outbound(bus, outbound);
}

/* Add task to active tasks list */
static void add_active_task(AgentLoop* loop, const char* task_id, const char* session_key, pthread_t thread) {
    pthread_mutex_lock(&loop->task_mutex);

    ActiveTaskNode* node = malloc(sizeof(ActiveTaskNode));
    if (node) {
        strncpy(node->task_id, task_id, sizeof(node->task_id) - 1);
        strncpy(node->session_key, session_key, sizeof(node->session_key) - 1);
        node->thread = thread;
        node->cancelling = false;
        node->next = loop->active_tasks;
        loop->active_tasks = node;
        log_debug("[AgentLoop] Added active task: %s", task_id);
    }

    pthread_mutex_unlock(&loop->task_mutex);
}

/* Remove task from active tasks list */
static void remove_active_task(AgentLoop* loop, const char* task_id) {
    pthread_mutex_lock(&loop->task_mutex);

    ActiveTaskNode* current = loop->active_tasks;
    ActiveTaskNode* prev = NULL;

    while (current) {
        if (strcmp(current->task_id, task_id) == 0) {
            if (prev) {
                prev->next = current->next;
            } else {
                loop->active_tasks = current->next;
            }
            free(current);
            log_debug("[AgentLoop] Removed active task: %s", task_id);
            break;
        }
        prev = current;
        current = current->next;
    }

    pthread_mutex_unlock(&loop->task_mutex);
}

/* Process a single message through the agent loop */
static Error process_message(AgentLoop* loop, InboundMessage* inbound, Session* session) {
    char key[256];
    snprintf(key, sizeof(key), "%s:%s", inbound->channel.data, inbound->chat_id.data);

    /* Store current session key for /stop */
    strncpy(loop->current_session_key, key, sizeof(loop->current_session_key) - 1);

    /* Set tool context */
    Tool* cron_tool = tool_registry_get(loop->tool_reg, "cron");
    if (cron_tool && cron_tool->user_data) {
        tool_context_set_route((ToolContext*)cron_tool->user_data,
                               inbound->channel.data, inbound->chat_id.data);
    }

    int max_turns = 15;  /* Default max iterations */
    int turn = 0;
    bool conversation_turn_done = false;
    bool error_occurred = false;

    log_debug("[AgentLoop] Processing message for session: %s", key);

    while (!conversation_turn_done && turn < max_turns && loop->running) {
        turn++;
        log_debug("[AgentLoop] Turn %d/%d", turn, max_turns);

        /* Build context with channel info */
        String system_prompt = context_builder_build_with_channel(
            loop->ctx_builder, session, loop->tool_reg,
            inbound->channel.data, inbound->chat_id.data
        );

        /* LLM call */
        String response = string_new("");
        ToolCall* tool_calls = NULL;
        size_t tool_calls_count = 0;

        Error err;
        if (loop->llm_call) {
            err = loop->llm_call(system_prompt.data, session, loop->tool_reg, loop->config, &response, &tool_calls, &tool_calls_count);
        } else {
            err = error_new(ERR_INVALID_PARAM, "No LLM provider set");
        }

        string_free(&system_prompt);

        if (err.code != ERR_NONE) {
            log_error("[AgentLoop] LLM call error: %s", err.message);
            send_error_response(loop, inbound->channel.data, inbound->chat_id.data, err.message);
            string_free(&response);
            error_occurred = true;
            break;
        }

        /* Strip think tags */
        char* clean_content = strip_think_tags(response.data);
        if (clean_content && strcmp(clean_content, response.data) != 0) {
            log_debug("[AgentLoop] Stripped think tags from response");
        }

        /* Check for tool calls */
        if (tool_calls_count == 0) {
            /* Final response - only send if no error */
            if (!error_occurred && clean_content && strlen(clean_content) > 0) {
                OutboundMessage* outbound = outbound_message_new(inbound->channel.data, inbound->chat_id.data, clean_content);
                message_bus_send_outbound(loop->bus, outbound);
            }
            if (clean_content) free(clean_content);
            string_free(&response);
            conversation_turn_done = true;
        } else {
            /* Add assistant message with tool calls */
            Message* assistant_msg = message_new(ROLE_ASSISTANT, clean_content ? clean_content : response.data);
            for (size_t i = 0; i < tool_calls_count; i++) {
                message_add_tool_call(assistant_msg, tool_calls[i].id.data, tool_calls[i].name.data, tool_calls[i].arguments.data);
            }
            session_add_message(session, assistant_msg);

            if (clean_content && clean_content != response.data) {
                free(clean_content);
            }

            /* Execute tools */
            for (size_t i = 0; i < tool_calls_count; i++) {
                String result = string_new("");

                /* Log tool execution (for debugging only) */
                log_debug("[AgentLoop] Executing tool: %s", tool_calls[i].name.data);

                err = tool_registry_execute(loop->tool_reg, tool_calls[i].name.data, tool_calls[i].arguments.data, &result);
                if (err.code != ERR_NONE) {
                    log_error("[AgentLoop] Tool Execution Failed: %s", err.message);

                    /* Check if this might be a skill that was incorrectly called as a tool */
                    /* Provide helpful error message to help LLM recover */
                    string_free(&result);
                    if (strstr(err.message, "not found") != NULL || strstr(err.message, "Unknown tool") != NULL) {
                        /* This might be a skill - suggest using skill tool */
                        char hint[512];
                        snprintf(hint, sizeof(hint),
                            "Error: '%s' is not a registered tool. "
                            "If this is a skill, use the `skill` tool first: {\"action\": \"load\", \"name\": \"%s\"}. "
                            "Then follow the skill's instructions to complete the task.",
                            tool_calls[i].name.data, tool_calls[i].name.data);
                        result = string_new(hint);
                        log_info("[AgentLoop] '%s' may be a skill - suggested: use `skill` tool to load it", tool_calls[i].name.data);
                    } else {
                        result = string_new("Tool execution failed");
                    }
                } else {
                    if (strcmp(tool_calls[i].name.data, "skill") == 0) {
                        log_info("[AgentLoop] Tool Result: [Skill content loaded, length: %zu bytes]", result.len);
                    } else {
                        log_info("[AgentLoop] Tool Result: %s", result.data);
                    }
                }

                Message* tool_msg = message_new(ROLE_TOOL, result.data);
                tool_msg->tool_call_id = string_copy(&tool_calls[i].id);
                tool_msg->name = string_copy(&tool_calls[i].name);
                session_add_message(session, tool_msg);
                string_free(&result);
            }

            string_free(&response);

            /* Cleanup tool calls array */
            for (size_t i = 0; i < tool_calls_count; i++) {
                string_free(&tool_calls[i].id);
                string_free(&tool_calls[i].name);
                string_free(&tool_calls[i].arguments);
            }
            free(tool_calls);

            /* Save session after tool execution */
            session_manager_save(loop->session_mgr, session);
        }
    }

    /* Check for max iterations */
    if (turn >= max_turns && !conversation_turn_done) {
        log_warn("[AgentLoop] Max iterations (%d) reached", max_turns);
        send_error_response(loop, inbound->channel.data, inbound->chat_id.data,
            "I reached the maximum number of tool call iterations without completing the task.");
    }

    /* Save session */
    if (!error_occurred) {
        session_manager_save(loop->session_mgr, session);
    }

    loop->current_session_key[0] = '\0';

    return error_new(ERR_NONE, "");
}

/* Main agent loop - receives and processes messages */
void agent_loop_run(AgentLoop* loop) {
    loop->running = true;
    log_info("[AgentLoop] Started");

    while (loop->running) {
        /* Receive inbound message */
        InboundMessage* inbound = message_bus_receive_inbound(loop->bus);
        if (!inbound) {
            usleep(100000); /* 100ms */
            continue;
        }

        /* Check for system exit command */
        if (strcmp(inbound->channel.data, "system") == 0 && strcmp(inbound->content.data, "exit") == 0) {
            log_info("[AgentLoop] System exit received, shutting down");
            loop->running = false;
            inbound_message_free(inbound);
            break;
        }

        /* Skip empty messages */
        if (inbound->content.len == 0) {
            inbound_message_free(inbound);
            continue;
        }

        /* Get or create session */
        char key[256];
        snprintf(key, sizeof(key), "%s:%s", inbound->channel.data, inbound->chat_id.data);
        Session* session = session_manager_get(loop->session_mgr, key);
        if (!session) {
            session_manager_load(loop->session_mgr, key, &session);
            log_debug("[AgentLoop] Loaded session: %s", key);
        }

        /* Check for slash commands */
        const char* content = inbound->content.data;
        if (content[0] == '/') {
            if (strcmp(content, "/stop") == 0 || strncmp(content, "/stop ", 6) == 0) {
                handle_stop(loop, key, inbound->channel.data, inbound->chat_id.data);
                inbound_message_free(inbound);
                continue;
            } else if (strcmp(content, "/restart") == 0) {
                handle_restart(loop, inbound->channel.data, inbound->chat_id.data);
                inbound_message_free(inbound);
                continue;
            } else if (strcmp(content, "/new") == 0) {
                handle_new(loop, inbound->channel.data, inbound->chat_id.data, session);
                inbound_message_free(inbound);
                continue;
            } else if (strcmp(content, "/help") == 0) {
                handle_help(inbound->channel.data, inbound->chat_id.data, loop->bus);
                inbound_message_free(inbound);
                continue;
            }
        }

        /* Add user message to session */
        Message* user_msg = message_new(ROLE_USER, inbound->content.data);
        session_add_message(session, user_msg);

        /* Generate task ID for tracking */
        char task_id[32];
        snprintf(task_id, sizeof(task_id), "task_%ld", time(NULL));
        pthread_t current_thread = pthread_self();
        add_active_task(loop, task_id, key, current_thread);

        /* Process message */
        process_message(loop, inbound, session);

        /* Remove from active tasks */
        remove_active_task(loop, task_id);

        inbound_message_free(inbound);
    }

    log_info("[AgentLoop] Stopped");
}
