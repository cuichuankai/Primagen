#include "agent_loop.h"
#include "../include/common.h"
#include "../include/logger.h"
#include "../tools/tools_impl.h"
#include "../tools/tool_executor.h"
#include "../tools/builtin_tools_def.h"
#include "../include/utils.h"
#include "../plugin/plugin_manager.h"
#include "../vendor/cJSON/cJSON.h"
#include <string.h>
#include <stdio.h>
#include <sys/time.h>

/* Forward declaration - defined in utils.c */
extern char* strip_think_tags(const char* text);

/* =============================================================================
   Command Runtime Context - Hide AgentLoop and transport internals from commands
   ============================================================================= */

typedef struct {
    AgentLoop* loop;
    const char* session_key;
    const char* channel;
    const char* chat_id;
} CommandRuntimeData;

/* =============================================================================
   Helper Functions
   ============================================================================= */

static void dynamic_array_clear_impl(DynamicArray* arr) {
    if (!arr) return;
    arr->count = 0;
}

static void send_error_response(AgentLoop* loop, const char* channel, const char* chat_id, const char* error_msg) {
    char full_msg[512];
    snprintf(full_msg, sizeof(full_msg), "Sorry, I encountered an error: %s", error_msg);
    OutboundMessage* outbound = outbound_message_new(channel, chat_id, full_msg);
    message_bus_send_outbound(loop->bus, outbound);
}

static bool agent_loop_is_running(AgentLoop* loop) {
    if (!loop) return false;
    return atomic_load(&loop->running);
}

static void agent_loop_set_running(AgentLoop* loop, bool running) {
    if (!loop) return;
    atomic_store(&loop->running, running);
}

static void maybe_auto_consolidate_memory(AgentLoop* loop, Session* session, const char* session_key, const char* latest_user_content) {
    if (!loop || !session || !loop->ctx_builder || !loop->ctx_builder->memory) return;

    size_t memory_window = 100;
    if (loop->config && loop->config->agent.memory_window > 0) {
        memory_window = (size_t) loop->config->agent.memory_window;
    }

    if (session->messages.count < session->last_consolidated) {
        session->last_consolidated = session->messages.count;
    }

    size_t delta = session->messages.count - session->last_consolidated;
    if (delta < memory_window) return;

    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &tm_info);

    char latest_excerpt[256] = {0};
    if (latest_user_content && latest_user_content[0] != '\0') {
        size_t n = strlen(latest_user_content);
        if (n > 220) n = 220;
        for (size_t i = 0; i < n; i++) {
            char c = latest_user_content[i];
            latest_excerpt[i] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
        }
        latest_excerpt[n] = '\0';
    } else {
        strcpy(latest_excerpt, "(empty)");
    }

    char history_entry[1024];
    snprintf(history_entry, sizeof(history_entry),
             "[%s] Auto consolidation for session %s: archived %zu messages since index %zu. Latest user message: %s",
             ts, session_key, delta, session->last_consolidated, latest_excerpt);

    Error append_err = memory_append_history(loop->ctx_builder->memory, loop->workspace_path, history_entry);
    if (append_err.code != ERR_NONE) {
        log_error("[AgentLoop] Auto memory append failed: %s", append_err.message);
        return;
    }

    Error consolidate_err = memory_consolidate(loop->ctx_builder->memory, loop->workspace_path);
    if (consolidate_err.code != ERR_NONE) {
        log_error("[AgentLoop] Auto memory consolidate failed: %s", consolidate_err.message);
        return;
    }

    session->last_consolidated = session->messages.count;
    Error save_err = session_manager_save(loop->session_mgr, session);
    if (save_err.code != ERR_NONE) {
        log_error("[AgentLoop] Failed to persist session consolidation cursor: %s", save_err.message);
    }
}

/* =============================================================================
   Built-in Command Definitions - Registered via PluginManager
   Note: Defined at runtime in agent_loop_register_builtin_commands()
   ============================================================================= */

// Command functions are defined in command_dispatcher.c

/* =============================================================================
   Built-in Tools - Registered via PluginManager
   Tool functions are defined in tools_impl.c
   ============================================================================= */

// Forward declarations of tool functions from tools_impl.c
extern Error tool_read_file(void* user_data, const char* args_json, String* result);
extern Error tool_write_file(void* user_data, const char* args_json, String* result);
extern Error tool_edit_file(void* user_data, const char* args_json, String* result);
extern Error tool_list_dir(void* user_data, const char* args_json, String* result);
extern Error tool_exec(void* user_data, const char* args_json, String* result);
extern Error tool_send_message(void* user_data, const char* args_json, String* result);
extern Error tool_spawn(void* user_data, const char* args_json, String* result);
extern Error tool_cron(void* user_data, const char* args_json, String* result);
extern Error tool_skill(void* user_data, const char* args_json, String* result);
extern Error tool_memory(void* user_data, const char* args_json, String* result);

// Note: builtin_tools array is defined at runtime in agent_loop_register_builtin_tools()
// because user_data (ToolContext*) is only available at runtime.

/* =============================================================================
   AgentLoop Lifecycle
   ============================================================================= */

AgentLoop* agent_loop_new(SessionManager* session_mgr, ContextBuilder* ctx_builder, ToolRegistry* tool_reg, MessageBus* bus, Config* config, PluginManager* plugin_mgr, const char* workspace_path) {
    AgentLoop* loop = malloc(sizeof(AgentLoop));
    if (!loop) return NULL;

    loop->session_mgr = session_mgr;
    loop->ctx_builder = ctx_builder;
    loop->tool_registry = tool_reg;
    loop->tool_executor = tool_executor_new(tool_reg, 0);
    loop->bus = bus;
    loop->config = config;
    loop->plugin_mgr = plugin_mgr;

    if (workspace_path) {
        strncpy(loop->workspace_path, workspace_path, sizeof(loop->workspace_path) - 1);
        loop->workspace_path[sizeof(loop->workspace_path) - 1] = '\0';
    } else {
        loop->workspace_path[0] = '\0';
    }

    atomic_store(&loop->running, false);
    loop->llm_call = NULL;
    loop->inbox_head = NULL;
    loop->inbox_tail = NULL;
    loop->processing_thread_started = false;
    pthread_mutex_init(&loop->inbox_mutex, NULL);
    pthread_cond_init(&loop->inbox_cond, NULL);

    // Create command dispatcher
    loop->command_dispatcher = command_dispatcher_new(loop);
    if (loop->command_dispatcher) {
        command_dispatcher_register_builtin_commands(loop->command_dispatcher);
    }

    // Create tool router
    loop->tool_router = tool_router_new(loop, loop->tool_registry);

    // Create session orchestrator
    loop->session_orchestrator = session_orchestrator_new(loop, loop->session_mgr);

    log_debug("[AgentLoop] Created new instance");
    return loop;
}

void agent_loop_free(AgentLoop* loop) {
    if (!loop) return;

    if (agent_loop_is_running(loop) || loop->processing_thread_started) {
        agent_loop_stop(loop);
        if (loop->processing_thread_started && !pthread_equal(pthread_self(), loop->processing_thread)) {
            pthread_join(loop->processing_thread, NULL);
            loop->processing_thread_started = false;
        }
    }

    if (loop->tool_executor) {
        tool_executor_destroy(loop->tool_executor);
    }

    // Free command dispatcher
    if (loop->command_dispatcher) {
        command_dispatcher_free(loop->command_dispatcher);
    }

    // Free tool router
    if (loop->tool_router) {
        tool_router_free(loop->tool_router);
    }

    // Free session orchestrator
    if (loop->session_orchestrator) {
        session_orchestrator_free(loop->session_orchestrator);
    }

    pthread_mutex_lock(&loop->inbox_mutex);
    InboundTaskNode* inbox_node = loop->inbox_head;
    while (inbox_node) {
        InboundTaskNode* next = inbox_node->next;
        if (inbox_node->inbound) {
            inbound_message_free(inbox_node->inbound);
        }
        free(inbox_node);
        inbox_node = next;
    }
    loop->inbox_head = NULL;
    loop->inbox_tail = NULL;
    pthread_mutex_unlock(&loop->inbox_mutex);
    pthread_mutex_destroy(&loop->inbox_mutex);
    pthread_cond_destroy(&loop->inbox_cond);
    log_debug("[AgentLoop] Freed instance");
    free(loop);
}

void agent_loop_set_llm_provider_async(AgentLoop* loop, LLMProviderAsync provider) {
    if (!loop) return;
    loop->llm_call_async = provider;
    log_debug("[AgentLoop] Async LLM provider set");
}

void agent_loop_stop(AgentLoop* loop) {
    if (!loop) return;
    atomic_store(&loop->running, false);
    
    pthread_mutex_lock(&loop->inbox_mutex);
    message_bus_close(loop->bus);
    pthread_cond_broadcast(&loop->inbox_cond);
    pthread_mutex_unlock(&loop->inbox_mutex);
    log_debug("[AgentLoop] Stop requested");
}

/* =============================================================================
   Tool Registration - Register built-in tools with PluginManager
   ============================================================================= */

void agent_loop_register_builtin_tools(PluginManager* manager, ToolContext* ctx) {
    log_debug("[AgentLoop] Registering built-in tools...");
    if (!manager || !ctx) {
        log_error("[AgentLoop] Invalid parameters for agent_loop_register_builtin_tools");
        return;
    }

    for (size_t i = 0; i < BUILTIN_TOOLS_FULL_COUNT; i++) {
        ToolContext* tool_ctx = malloc(sizeof(ToolContext));
        if (!tool_ctx) {
            log_error("[AgentLoop] Failed to allocate ToolContext for tool: %s", BUILTIN_TOOLS_FULL[i].name);
            continue;
        }
        memcpy(tool_ctx, ctx, sizeof(ToolContext));
        pthread_mutex_init(&tool_ctx->route_mutex, NULL);
        log_debug("[AgentLoop] Created ToolContext for %s: ptr=%p, magic=0x%x, bus=%p", 
                  BUILTIN_TOOLS_FULL[i].name, (void*)tool_ctx, tool_ctx->magic, (void*)tool_ctx->bus);

        int result = plugin_register_tool_with_destroy(manager, NULL,
            BUILTIN_TOOLS_FULL[i].name,
            BUILTIN_TOOLS_FULL[i].desc,
            BUILTIN_TOOLS_FULL[i].params,
            BUILTIN_TOOLS_FULL[i].exec,
            tool_ctx,
            tool_context_destroy);

        if (result != 0) {
            log_error("[AgentLoop] Failed to register built-in tool: %s", BUILTIN_TOOLS_FULL[i].name);
            free(tool_ctx);
        }
    }

    log_debug("[AgentLoop] Registered %zu built-in tools", BUILTIN_TOOLS_FULL_COUNT);
}

void agent_loop_register_builtin_commands(AgentLoop* loop) {
    if (!loop || !loop->command_dispatcher) return;
    command_dispatcher_register_builtin_commands(loop->command_dispatcher);
}

/* =============================================================================
   Channel Registration - Register built-in channels with PluginManager
   ============================================================================= */

void agent_loop_register_builtin_channels(PluginManager* manager, Config* cfg) {
    log_debug("[AgentLoop] Registering built-in channels...");
    if (!manager) {
        log_error("[AgentLoop] PluginManager is NULL");
        return;
    }

    // Define built-in channels (Console only - Feishu is now a plugin)
    struct {
        const char* name;
        ChannelCreateFunc create;
        const char* config_section;  // Config section name for enabling
    } builtin_channels[] = {
        {"console", channel_create_console, NULL},  // Always enabled
    };
    #define BUILTIN_CHANNELS_COUNT (sizeof(builtin_channels) / sizeof(builtin_channels[0]))

    for (size_t i = 0; i < BUILTIN_CHANNELS_COUNT; i++) {
        log_debug("[AgentLoop] Processing built-in channel: %s", builtin_channels[i].name);
        // Console channel is always enabled (config_section is NULL)

        int result = plugin_register_channel(manager, NULL,
            builtin_channels[i].name,
            builtin_channels[i].create);

        if (result != 0) {
            log_error("[AgentLoop] Failed to register built-in channel: %s", builtin_channels[i].name);
        }
    }

    log_debug("[AgentLoop] Registered %zu built-in channels", BUILTIN_CHANNELS_COUNT);
}

/* =============================================================================
   Plugin Command Handler - Unified handling for all commands
   ============================================================================= */

static void process_inbound_message(AgentLoop* loop, InboundMessage* inbound);
static void enqueue_inbound_task(AgentLoop* loop, InboundMessage* inbound);
static InboundMessage* dequeue_inbound_task(AgentLoop* loop);
static void* agent_loop_processing_worker(void* arg);

/* =============================================================================
   Task Tracking
   ============================================================================= */

void add_active_task(AgentLoop* loop, const char* task_id, const char* session_key, pthread_t thread, InboundMessage* msg) {
    if (!loop || !loop->session_orchestrator) return;
    
    ActiveTask* task = calloc(1, sizeof(ActiveTask));
    if (!task) return;
    
    strncpy(task->id, task_id, sizeof(task->id) - 1);
    task->id[sizeof(task->id) - 1] = '\0';
    strncpy(task->session_key, session_key, sizeof(task->session_key) - 1);
    task->session_key[sizeof(task->session_key) - 1] = '\0';
    task->thread = thread;
    task->cancelled = false;
    task->pending_tool_count = 0;
    task->tool_calls = NULL;
    task->tool_calls_count = 0;
    
    task->ctx.state = SESSION_STATE_IDLE;
    task->ctx.turn = 0;
    if (msg) {
        strncpy(task->ctx.channel, msg->channel.data, sizeof(task->ctx.channel) - 1);
        task->ctx.channel[sizeof(task->ctx.channel) - 1] = '\0';
        strncpy(task->ctx.chat_id, msg->chat_id.data, sizeof(task->ctx.chat_id) - 1);
        task->ctx.chat_id[sizeof(task->ctx.chat_id) - 1] = '\0';
        strncpy(task->ctx.latest_user_content, msg->content.data, sizeof(task->ctx.latest_user_content) - 1);
        task->ctx.latest_user_content[sizeof(task->ctx.latest_user_content) - 1] = '\0';
    }
    
    session_orchestrator_add_task(loop->session_orchestrator, task);
}

static void remove_active_task(AgentLoop* loop, const char* task_id) {
    if (!loop || !loop->session_orchestrator) return;
    
    // Remove task from session orchestrator
    session_orchestrator_remove_task(loop->session_orchestrator, task_id);
}

void refresh_tool_routes(AgentLoop* loop, const char* channel, const char* chat_id) {
    log_debug("[AgentLoop] refresh_tool_routes: channel=%s, chat_id=%s", channel, chat_id);
    const char* tool_names[] = {"cron", "send_message", "spawn_subagent", "skill", "memory", "exec"};
    size_t count = sizeof(tool_names) / sizeof(tool_names[0]);
    for (size_t i = 0; i < count; i++) {
        Tool* tool = tool_registry_get(loop->tool_registry, tool_names[i]);
        log_debug("[AgentLoop] Tool %s: tool=%p, user_data=%p", tool_names[i], (void*)tool, tool ? (void*)tool->user_data : NULL);
        if (tool && tool->user_data) {
            tool_context_set_route((ToolContext*)tool->user_data, channel, chat_id);
        }
    }
}

/* =============================================================================
   Message Processing
   ============================================================================= */

typedef struct {
    AgentLoop* loop;
    char session_key[256];
} AsyncContext;

static void handle_llm_callback(Error err, const char* response, ToolCall* tool_calls, size_t tool_calls_count, void* user_data) {
    AsyncContext* ctx = (AsyncContext*)user_data;
    if (!ctx || !ctx->loop || !ctx->loop->bus) return;

    InternalEvent* event = internal_event_new_llm_result(ctx->session_key, err, response, tool_calls, tool_calls_count);
    message_bus_send_internal(ctx->loop->bus, event);
    free(ctx);
}

void trigger_llm_async(AgentLoop* loop, Session* session, const char* session_key, const char* channel, const char* chat_id) {
    uint64_t start_ms = 0;
    { struct timeval tv; gettimeofday(&tv, NULL); start_ms = (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000; }
    log_debug("[AgentLoop] Building context for session %s...", session_key);
    
    String system_prompt = context_builder_build_with_channel(
        loop->ctx_builder, session, loop->tool_registry, channel, chat_id
    );
    
    uint64_t ctx_end_ms = 0;
    { struct timeval tv; gettimeofday(&tv, NULL); ctx_end_ms = (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000; }
    log_debug("[AgentLoop] Context built in %llu ms, system prompt length: %zu", 
              (unsigned long long)(ctx_end_ms - start_ms), system_prompt.len);

    AsyncContext* ctx = malloc(sizeof(AsyncContext));
    if (!ctx) {
        log_error("[AgentLoop] Failed to allocate AsyncContext");
        string_free(&system_prompt);
        return;
    }
    ctx->loop = loop;
    strncpy(ctx->session_key, session_key, sizeof(ctx->session_key) - 1);
    ctx->session_key[sizeof(ctx->session_key) - 1] = '\0';

    uint64_t llm_start_ms = 0;
    { struct timeval tv; gettimeofday(&tv, NULL); llm_start_ms = (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000; }
    log_debug("[AgentLoop] Calling LLM async provider...");
    
    if (loop->llm_call_async) {
        loop->llm_call_async(system_prompt.data, session, loop->tool_registry, loop->config, handle_llm_callback, ctx);
    } else {
        log_error("[AgentLoop] No async LLM provider configured");
        handle_llm_callback(error_new(ERR_INVALID_PARAM, "No async LLM provider configured"), NULL, NULL, 0, ctx);
    }

    uint64_t llm_end_ms = 0;
    { struct timeval tv; gettimeofday(&tv, NULL); llm_end_ms = (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000; }
    log_debug("[AgentLoop] LLM provider called in %llu ms", (unsigned long long)(llm_end_ms - llm_start_ms));

    string_free(&system_prompt);
}

// =============================================================================
// State Machine Event Handlers
// =============================================================================

// Forward declarations for tool async support
static void tool_executor_callback(Error err, const char* result, void* user_data);

typedef struct {
    AgentLoop* loop;
    char session_key[256];
    char tool_call_id[128];
    char tool_name[128];
} ToolAsyncContext;

static void tool_executor_callback(Error err, const char* result, void* user_data) {
    ToolAsyncContext* ctx = (ToolAsyncContext*)user_data;
    if (!ctx || !ctx->loop || !ctx->loop->bus) return;

    InternalEvent* event = internal_event_new_tool_result(ctx->session_key, ctx->tool_call_id, ctx->tool_name, result, err);
    message_bus_send_internal(ctx->loop->bus, event);
    free(ctx);
}

static void handle_event_llm_result(AgentLoop* loop, InternalEvent* event) {
    TaskSnapshot snap = session_orchestrator_snapshot_task(loop->session_orchestrator, event->session_key.data);
    if (!snap.valid) return;
    
    if (snap.cancelled) {
        log_info("[AgentLoop] Task %s was cancelled, discarding LLM result", snap.task_id);
        remove_active_task(loop, snap.task_id);
        return;
    }
    
    Session* session = session_manager_get(loop->session_mgr, event->session_key.data);
    if (!session) {
        log_error("[AgentLoop] Session not found for key: %s", event->session_key.data);
        remove_active_task(loop, snap.task_id);
        return;
    }
    
    if (event->llm_error.code != ERR_NONE) {
        log_error("[AgentLoop] LLM call error: %s", event->llm_error.message);
        char full_msg[512];
        snprintf(full_msg, sizeof(full_msg), "Sorry, I encountered an error: %s", event->llm_error.message);
        Message* assistant_msg = message_new(ROLE_ASSISTANT, full_msg);
        session_add_message(session, assistant_msg);
        session_manager_save(loop->session_mgr, session);
        OutboundMessage* outbound = outbound_message_new(snap.channel, snap.chat_id, full_msg);
        message_bus_send_outbound(loop->bus, outbound);
        
        remove_active_task(loop, snap.task_id);
        return;
    }

    char* clean_content = strip_think_tags(event->llm_response.data);
    bool clean_content_owned = (clean_content != NULL && clean_content != event->llm_response.data);

    if (event->tool_calls_count == 0) {
        const char* assistant_content = (clean_content && strlen(clean_content) > 0) ? clean_content : event->llm_response.data;
        if (assistant_content && strlen(assistant_content) > 0) {
            Message* assistant_msg = message_new(ROLE_ASSISTANT, assistant_content);
            session_add_message(session, assistant_msg);
            session_manager_save(loop->session_mgr, session);
        }
        
        const char* final_out = (assistant_content && strlen(assistant_content) > 0) ? assistant_content : "";
        if (final_out && strlen(final_out) > 0) {
            OutboundMessage* outbound = outbound_message_new(snap.channel, snap.chat_id, final_out);
            message_bus_send_outbound(loop->bus, outbound);
        } else {
            OutboundMessage* outbound = outbound_message_new(snap.channel, snap.chat_id, "");
            message_bus_send_outbound(loop->bus, outbound);
        }
        
        if (clean_content_owned) free(clean_content);
        
        remove_active_task(loop, snap.task_id);
    } else {
        for (size_t i = 0; i < event->tool_calls_count; i++) {
            if (tool_registry_get(loop->tool_registry, event->tool_calls[i].name.data) == NULL &&
                strcmp(event->tool_calls[i].name.data, "skill") != 0) {
                char skill_args[256];
                char original_name[128];
                snprintf(original_name, sizeof(original_name), "%s", event->tool_calls[i].name.data);
                snprintf(skill_args, sizeof(skill_args), "{\"action\":\"load\",\"name\":\"%s\"}", original_name);
                string_free(&event->tool_calls[i].name);
                event->tool_calls[i].name = string_new("skill");
                string_free(&event->tool_calls[i].arguments);
                event->tool_calls[i].arguments = string_new(skill_args);
                log_info("[AgentLoop] Rewriting unresolved tool '%s' to skill load", original_name);
            }
        }

        if (clean_content && strlen(clean_content) > 0) {
            OutboundMessage* progress = outbound_message_new(snap.channel, snap.chat_id, clean_content);
            message_bus_send_outbound(loop->bus, progress);
        }

        Message* assistant_msg = message_new(ROLE_ASSISTANT, clean_content ? clean_content : event->llm_response.data);
        for (size_t i = 0; i < event->tool_calls_count; i++) {
            message_add_tool_call(assistant_msg, event->tool_calls[i].id.data, event->tool_calls[i].name.data, event->tool_calls[i].arguments.data);
        }
        session_add_message(session, assistant_msg);
        session_manager_save(loop->session_mgr, session);

        if (clean_content_owned) free(clean_content);

        session_orchestrator_update_task_state(loop->session_orchestrator, event->session_key.data, SESSION_STATE_WAITING_TOOL);
        session_orchestrator_update_task_pending_tools(loop->session_orchestrator, event->session_key.data, (int)event->tool_calls_count);
        
        refresh_tool_routes(loop, snap.channel, snap.chat_id);
        
        for (size_t i = 0; i < event->tool_calls_count; i++) {
            ToolAsyncContext* tctx = malloc(sizeof(ToolAsyncContext));
            if (!tctx) {
                log_error("[AgentLoop] Failed to allocate ToolAsyncContext for tool %s", event->tool_calls[i].name.data);
                InternalEvent* err_event = internal_event_new_tool_result(
                    event->session_key.data,
                    event->tool_calls[i].id.data,
                    event->tool_calls[i].name.data,
                    "Internal error: out of memory",
                    error_new(ERR_MEMORY, "Failed to allocate tool context")
                );
                message_bus_send_internal(loop->bus, err_event);
                continue;
            }
            tctx->loop = loop;
            strncpy(tctx->session_key, event->session_key.data, sizeof(tctx->session_key) - 1);
            tctx->session_key[sizeof(tctx->session_key) - 1] = '\0';
            strncpy(tctx->tool_call_id, event->tool_calls[i].id.data, sizeof(tctx->tool_call_id) - 1);
            tctx->tool_call_id[sizeof(tctx->tool_call_id) - 1] = '\0';
            strncpy(tctx->tool_name, event->tool_calls[i].name.data, sizeof(tctx->tool_name) - 1);
            tctx->tool_name[sizeof(tctx->tool_name) - 1] = '\0';
            
            tool_executor_submit_async(loop->tool_executor, event->tool_calls[i].name.data, event->tool_calls[i].arguments.data, tool_executor_callback, tctx);
        }
    }
}

static void handle_event_tool_result(AgentLoop* loop, InternalEvent* event) {
    TaskSnapshot snap = session_orchestrator_snapshot_task(loop->session_orchestrator, event->session_key.data);
    if (!snap.valid) return;

    if (snap.cancelled) {
        log_info("[AgentLoop] Task %s was cancelled, discarding tool result", snap.task_id);
        int remaining = session_orchestrator_decrement_task_pending_tools(loop->session_orchestrator, event->session_key.data);
        if (remaining <= 0) {
            remove_active_task(loop, snap.task_id);
        }
        return;
    }

    Session* session = session_manager_get(loop->session_mgr, event->session_key.data);
    if (!session) {
        log_error("[AgentLoop] Session not found for tool result key: %s", event->session_key.data);
        remove_active_task(loop, snap.task_id);
        return;
    }

    String result = string_new("");
    if (event->tool_error.code != ERR_NONE) {
        log_error("[AgentLoop] Tool Execution Failed: name=%s error=%s", event->tool_name.data, event->tool_error.message);
        string_append(&result, event->tool_error.message);
    } else {
        if (strcmp(event->tool_name.data, "skill") == 0) {
            log_debug("[AgentLoop] Tool Result: [Skill content loaded, length: %zu bytes]", event->tool_result.len);
        } else {
            log_debug("[AgentLoop] Tool Result: %s", event->tool_result.data);
        }
        string_append(&result, event->tool_result.data);
    }

    Message* tool_msg = message_new(ROLE_TOOL, result.data);
    tool_msg->tool_call_id = string_copy(&event->tool_call_id);
    tool_msg->name = string_copy(&event->tool_name);
    session_add_message(session, tool_msg);
    string_free(&result);
    session_manager_save(loop->session_mgr, session);

    int remaining = session_orchestrator_decrement_task_pending_tools(loop->session_orchestrator, event->session_key.data);
    if (remaining > 0) {
        log_debug("[AgentLoop] Tool result received, %d tools still pending", remaining);
        return;
    }
    
    int max_turns = loop->config && loop->config->agent.max_tool_iterations > 0 ? loop->config->agent.max_tool_iterations : 15;
    if (snap.turn >= max_turns) {
        log_warn("[AgentLoop] Max iterations (%d) reached", max_turns);
        send_error_response(loop, snap.channel, snap.chat_id, "I reached the maximum number of tool call iterations without completing the task.");
        remove_active_task(loop, snap.task_id);
        return;
    }

    session_orchestrator_increment_task_turn(loop->session_orchestrator, event->session_key.data);
    session_orchestrator_update_task_state(loop->session_orchestrator, event->session_key.data, SESSION_STATE_WAITING_LLM);
    trigger_llm_async(loop, session, event->session_key.data, snap.channel, snap.chat_id);
}

static void process_message_async(AgentLoop* loop, InboundMessage* inbound, Session* session) {
    char key[256];
    snprintf(key, sizeof(key), "%s:%s", inbound->channel.data, inbound->chat_id.data);

    session_orchestrator_set_current_session_key(loop->session_orchestrator, key);

    refresh_tool_routes(loop, inbound->channel.data, inbound->chat_id.data);

    TaskSnapshot snap = session_orchestrator_snapshot_task(loop->session_orchestrator, key);
    if (!snap.valid) return;

    if (snap.state != SESSION_STATE_IDLE) {
        log_warn("[AgentLoop] Session %s is busy", key);
        return;
    }

    log_debug("[AgentLoop] Processing message for session: %s", key);
    
    uint64_t start_ms = 0;
    { struct timeval tv; gettimeofday(&tv, NULL); start_ms = (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000; }

    session_orchestrator_update_task_state(loop->session_orchestrator, key, SESSION_STATE_WAITING_LLM);
    session_orchestrator_increment_task_turn(loop->session_orchestrator, key);
    
    uint64_t llm_start_ms = 0;
    { struct timeval tv; gettimeofday(&tv, NULL); llm_start_ms = (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000; }
    log_debug("[AgentLoop] Triggering LLM async...");
    
    trigger_llm_async(loop, session, key, inbound->channel.data, inbound->chat_id.data);
    
    uint64_t end_ms = 0;
    { struct timeval tv; gettimeofday(&tv, NULL); end_ms = (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000; }
    log_debug("[AgentLoop] LLM async triggered in %llu ms, total setup time: %llu ms", 
              (unsigned long long)(end_ms - llm_start_ms), 
              (unsigned long long)(end_ms - start_ms));
}

static void process_inbound_message(AgentLoop* loop, InboundMessage* inbound) {
    if (!loop || !inbound) return;
    if (inbound->content.len == 0) return;

    char key[256];
    snprintf(key, sizeof(key), "%s:%s", inbound->channel.data, inbound->chat_id.data);
    Session* session = session_manager_get(loop->session_mgr, key);
    if (!session) {
        session_manager_load(loop->session_mgr, key, &session);
        log_debug("[AgentLoop] Loaded session: %s", key);
    }
    if (!session) {
        OutboundMessage* outbound = outbound_message_new(inbound->channel.data, inbound->chat_id.data, "Session unavailable.");
        message_bus_send_outbound(loop->bus, outbound);
        return;
    }

    const char* content = inbound->content.data;
    const char* trimmed_content = str_trim_left(content);
    if (trimmed_content[0] == '/') {
        if (loop->command_dispatcher) {
            command_dispatcher_handle_message(loop->command_dispatcher, inbound, session, key, trimmed_content);
        } else {
            char response[256];
            snprintf(response, sizeof(response), "Command dispatcher not initialized. Type /help for available commands.");
            OutboundMessage* outbound = outbound_message_new(inbound->channel.data, inbound->chat_id.data, response);
            message_bus_send_outbound(loop->bus, outbound);
        }
        return;
    }

    char* formatted_content = NULL;
    if (inbound->sender_name.data && inbound->sender_name.len > 0) {
        size_t len = inbound->sender_name.len + inbound->content.len + 8;
        formatted_content = malloc(len);
        if (formatted_content) {
            snprintf(formatted_content, len, "[%s]: %s", inbound->sender_name.data, inbound->content.data);
        }
    }
    const char* msg_content = formatted_content ? formatted_content : inbound->content.data;

    Message* user_msg = message_new(ROLE_USER, msg_content);
    session_add_message(session, user_msg);
    maybe_auto_consolidate_memory(loop, session, key, msg_content);

    char task_id[64];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned int seed = (unsigned int)(ts.tv_nsec ^ (unsigned long)pthread_self());
    unsigned int rand_val = rand_r(&seed) % 1000000;
    snprintf(task_id, sizeof(task_id), "task_%ld_%u", (long)ts.tv_sec, rand_val);
    pthread_t current_thread = pthread_self();
    add_active_task(loop, task_id, key, current_thread, inbound);
    process_message_async(loop, inbound, session);

    if (formatted_content) free(formatted_content);
}

static void enqueue_inbound_task(AgentLoop* loop, InboundMessage* inbound) {
    if (!loop || !inbound) return;
    InboundTaskNode* node = malloc(sizeof(InboundTaskNode));
    if (!node) {
        inbound_message_free(inbound);
        return;
    }
    node->inbound = inbound;
    node->next = NULL;

    pthread_mutex_lock(&loop->inbox_mutex);
    if (loop->inbox_tail) {
        loop->inbox_tail->next = node;
    } else {
        loop->inbox_head = node;
    }
    loop->inbox_tail = node;
    pthread_cond_signal(&loop->inbox_cond);
    pthread_mutex_unlock(&loop->inbox_mutex);
}

static InboundMessage* dequeue_inbound_task(AgentLoop* loop) {
    if (!loop) return NULL;
    pthread_mutex_lock(&loop->inbox_mutex);
    // Don't wait here if we want to poll internal queue, use timedwait
    struct timespec ts;
#if defined(__APPLE__)
    ts.tv_sec = 0;
    ts.tv_nsec = 10000000; // 10ms
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_nsec += 10000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000;
    }
#endif

    while (loop->inbox_head == NULL && agent_loop_is_running(loop)) {
#if defined(__APPLE__)
        int rc = pthread_cond_timedwait_relative_np(&loop->inbox_cond, &loop->inbox_mutex, &ts);
#else
        int rc = pthread_cond_timedwait(&loop->inbox_cond, &loop->inbox_mutex, &ts);
#endif
        if (rc != 0) break; // timeout or error, break to allow checking internal queue
    }
    
    if (loop->inbox_head == NULL) {
        pthread_mutex_unlock(&loop->inbox_mutex);
        return NULL;
    }
    InboundTaskNode* node = loop->inbox_head;
    loop->inbox_head = node->next;
    if (loop->inbox_head == NULL) {
        loop->inbox_tail = NULL;
    }
    pthread_mutex_unlock(&loop->inbox_mutex);

    InboundMessage* inbound = node->inbound;
    free(node);
    return inbound;
}

static void* agent_loop_processing_worker(void* arg) {
    AgentLoop* loop = (AgentLoop*)arg;
    log_debug("[AgentLoop] Processing worker started");
    while (true) {
        // First check internal events (higher priority)
        InternalEvent* event = message_bus_receive_internal_timed(loop->bus, 0);
        if (event) {
            if (event->type == EVENT_LLM_RESULT) {
                handle_event_llm_result(loop, event);
            } else if (event->type == EVENT_TOOL_RESULT) {
                handle_event_tool_result(loop, event);
            }
            internal_event_free(event);
            continue;
        }

        InboundMessage* inbound = dequeue_inbound_task(loop);
        if (!inbound) {
            if (!agent_loop_is_running(loop)) break;
            
            // Wait for events if queue is empty
            event = message_bus_receive_internal_timed(loop->bus, 50);
            if (event) {
                if (event->type == EVENT_LLM_RESULT) {
                    handle_event_llm_result(loop, event);
                } else if (event->type == EVENT_TOOL_RESULT) {
                    handle_event_tool_result(loop, event);
                }
                internal_event_free(event);
            }
            continue;
        }
        process_inbound_message(loop, inbound);
        inbound_message_free(inbound);
    }
    return NULL;
}

void agent_loop_run(AgentLoop* loop) {
    agent_loop_set_running(loop, true);
    if (pthread_create(&loop->processing_thread, NULL, agent_loop_processing_worker, loop) != 0) {
        agent_loop_set_running(loop, false);
        log_error("[AgentLoop] Failed to start processing worker");
        return;
    }
    loop->processing_thread_started = true;

    log_debug("[AgentLoop] Started");
    while (agent_loop_is_running(loop)) {
        InboundMessage* inbound = message_bus_receive_inbound_timed(loop->bus, 200);
        if (!inbound) continue;

        if (strcmp(inbound->channel.data, "system") == 0 &&
            inbound->content.data && strcmp(inbound->content.data, "exit") == 0) {
            log_debug("[AgentLoop] System exit received, shutting down");
            agent_loop_set_running(loop, false);
            inbound_message_free(inbound);
            break;
        }

        if (!agent_loop_is_running(loop)) {
            inbound_message_free(inbound);
            break;
        }
        enqueue_inbound_task(loop, inbound);
    }

    pthread_mutex_lock(&loop->inbox_mutex);
    pthread_cond_broadcast(&loop->inbox_cond);
    pthread_mutex_unlock(&loop->inbox_mutex);
    pthread_join(loop->processing_thread, NULL);
    loop->processing_thread_started = false;
    log_debug("[AgentLoop] Stopped");
}
