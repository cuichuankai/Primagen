#include "agent_loop.h"
#include "../include/common.h"
#include "../include/logger.h"
#include "../tools/tools_impl.h"
#include "../tools/tool_executor.h"
#include "../include/utils.h"
#include "../plugin/plugin_manager.h"
#include "../vendor/cJSON/cJSON.h"
#include <string.h>
#include <stdio.h>

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
    bool running = false;
    pthread_mutex_lock(&loop->state_mutex);
    running = loop->running;
    pthread_mutex_unlock(&loop->state_mutex);
    return running;
}

static void agent_loop_set_running(AgentLoop* loop, bool running) {
    pthread_mutex_lock(&loop->state_mutex);
    loop->running = running;
    pthread_mutex_unlock(&loop->state_mutex);
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
   Built-in Commands - Follow CommandFunc signature exactly
   Context passed via CommandContext and user arguments in argv
   ============================================================================= */

static int command_ctx_send_response(CommandContext* ctx, const char* message) {
    if (!ctx || !ctx->user_data || !message) return -1;
    CommandRuntimeData* data = (CommandRuntimeData*)ctx->user_data;
    if (!data->loop || !data->loop->bus || !data->channel || !data->chat_id) return -1;
    OutboundMessage* outbound = outbound_message_new(data->channel, data->chat_id, message);
    message_bus_send_outbound(data->loop->bus, outbound);
    return 0;
}

static int command_ctx_stop_active_tasks(CommandContext* ctx) {
    if (!ctx || !ctx->user_data) return -1;
    CommandRuntimeData* data = (CommandRuntimeData*)ctx->user_data;
    if (!data->loop || !data->session_key) return 0;

    int cancelled = 0;
    pthread_mutex_lock(&data->loop->task_mutex);
    ActiveTaskNode* current = data->loop->active_tasks;
    while (current) {
        if (strcmp(current->session_key, data->session_key) == 0 && !current->cancelling) {
            current->cancelling = true;
            cancelled++;
        }
        current = current->next;
    }
    pthread_mutex_unlock(&data->loop->task_mutex);
    return cancelled;
}

static int command_ctx_reset_session(CommandContext* ctx) {
    if (!ctx || !ctx->user_data) return -1;
    CommandRuntimeData* data = (CommandRuntimeData*)ctx->user_data;
    if (!data->loop || !data->loop->session_mgr || !data->session_key) return -1;

    Session* session = session_manager_get(data->loop->session_mgr, data->session_key);
    if (!session) session_manager_load(data->loop->session_mgr, data->session_key, &session);
    if (!session) return -1;

    pthread_mutex_lock(&session->mutex);
    for (size_t i = 0; i < session->messages.count; i++) {
        Message* msg = *(Message**)dynamic_array_get(&session->messages, i);
        message_free(msg);
    }
    dynamic_array_clear_impl(&session->messages);
    session->last_consolidated = 0;
    session->updated_at = time(NULL);
    pthread_mutex_unlock(&session->mutex);
    session_manager_save(data->loop->session_mgr, session);
    return 0;
}

static CommandPluginDef* command_ctx_get_registered_commands(CommandContext* ctx, size_t* out_count) {
    if (!ctx || !ctx->user_data || !out_count) return NULL;
    CommandRuntimeData* data = (CommandRuntimeData*)ctx->user_data;
    if (!data->loop || !data->loop->plugin_mgr) {
        *out_count = 0;
        return NULL;
    }
    return plugin_manager_get_commands(data->loop->plugin_mgr, out_count);
}

static ToolRegistry* command_ctx_get_tool_registry(CommandContext* ctx) {
    if (!ctx || !ctx->user_data) return NULL;
    CommandRuntimeData* data = (CommandRuntimeData*)ctx->user_data;
    if (!data->loop) return NULL;
    return data->loop->tool_reg;
}

static PluginManager* command_ctx_get_plugin_manager(CommandContext* ctx) {
    if (!ctx || !ctx->user_data) return NULL;
    CommandRuntimeData* data = (CommandRuntimeData*)ctx->user_data;
    if (!data->loop) return NULL;
    return data->loop->plugin_mgr;
}

static int cmd_stop(CommandContext* ctx, Config* cfg, const char* workspace_path, int argc, char** argv) {
    (void)cfg;
    (void)workspace_path;
    (void)argc;
    (void)argv;

    int cancelled = ctx && ctx->stop_active_tasks ? ctx->stop_active_tasks(ctx) : -1;
    if (cancelled < 0) return -1;

    char response[256];
    snprintf(response, sizeof(response), cancelled > 0 ? "Stopped %d task(s)." : "No active task to stop.", cancelled);
    if (!ctx || !ctx->send_response) return -1;
    ctx->send_response(ctx, response);
    return cancelled;
}

static int cmd_restart(CommandContext* ctx, Config* cfg, const char* workspace_path, int argc, char** argv) {
    (void)cfg;
    (void)workspace_path;
    (void)argc;
    (void)argv;
    if (!ctx || !ctx->send_response) return -1;
    ctx->send_response(ctx, "Restarting...");
    log_debug("[AgentLoop] Restart requested but requires external wrapper");
    return 0;
}

static int cmd_new(CommandContext* ctx, Config* cfg, const char* workspace_path, int argc, char** argv) {
    (void)cfg;
    (void)workspace_path;
    (void)argc;
    (void)argv;
    if (!ctx || !ctx->reset_session || !ctx->send_response) return -1;
    if (ctx->reset_session(ctx) != 0) return -1;
    ctx->send_response(ctx, "New session started.");
    return 0;
}

static int cmd_help(CommandContext* ctx, Config* cfg, const char* workspace_path, int argc, char** argv) {
    (void)cfg;
    (void)workspace_path;
    (void)argc;
    (void)argv;
    if (!ctx || !ctx->get_registered_commands || !ctx->send_response) return -1;

    size_t command_count = 0;
    CommandPluginDef* commands = ctx->get_registered_commands(ctx, &command_count);

    String help_text = string_new("");
    string_append(&help_text, "\n      Primagen(Primitive Genesis) - AI Agent Framework  \n");
    string_append(&help_text, "===================================================================  \n");
    string_append(&help_text, "Primagen commands:  \n");

    if (commands && command_count > 0) {
        for (size_t i = 0; i < command_count; i++) {
            string_append(&help_text, "/");
            string_append(&help_text, commands[i].name);
            string_append(&help_text, " - ");
            string_append(&help_text, commands[i].description);
            if (i < command_count - 1) {
                string_append(&help_text, "  \n");
            }
        }
    } else {
        string_append(&help_text, "No commands registered.");
    }

    ctx->send_response(ctx, help_text.data);
    string_free(&help_text);
    return 0;
}

static int cmd_tools(CommandContext* ctx, Config* cfg, const char* workspace_path, int argc, char** argv) {
    (void)cfg;
    (void)workspace_path;
    (void)argc;
    (void)argv;
    if (!ctx || !ctx->get_tool_registry || !ctx->send_response) return -1;

    ToolRegistry* tool_reg = ctx->get_tool_registry(ctx);
    if (!tool_reg) return -1;

    String out = string_new("");
    string_append(&out, "\nAvailable tools:\n");

    size_t total = tool_reg->count;
    size_t builtin_count = 0;
    size_t plugin_count = 0;
    size_t mcp_count = 0;
    for (size_t i = 0; i < total; i++) {
        Tool* t = &tool_reg->tools[i];
        const char* name = t->def.name.data ? t->def.name.data : "";
        bool is_mcp = strncmp(name, "primagen_", 9) == 0 || strncmp(name, "mcp_", 4) == 0;
        if (is_mcp) {
            mcp_count++;
        } else if (t->plugin_ref) {
            plugin_count++;
        } else {
            builtin_count++;
        }
    }

    char summary[128];
    snprintf(summary, sizeof(summary), "Total: %zu (builtin: %zu, plugin: %zu, mcp: %zu)\n",
             total, builtin_count, plugin_count, mcp_count);
    string_append(&out, summary);

    if (builtin_count > 0) {
        string_append(&out, "\n[builtin]\n");
        for (size_t i = 0; i < total; i++) {
            Tool* t = &tool_reg->tools[i];
            const char* name = t->def.name.data ? t->def.name.data : "";
            bool is_mcp = strncmp(name, "primagen_", 9) == 0 || strncmp(name, "mcp_", 4) == 0;
            if (!is_mcp && !t->plugin_ref) {
                string_append(&out, "- ");
                string_append(&out, name);
                string_append(&out, "\n");
            }
        }
    }

    if (plugin_count > 0) {
        string_append(&out, "\n[plugin]\n");
        for (size_t i = 0; i < total; i++) {
            Tool* t = &tool_reg->tools[i];
            const char* name = t->def.name.data ? t->def.name.data : "";
            bool is_mcp = strncmp(name, "primagen_", 9) == 0 || strncmp(name, "mcp_", 4) == 0;
            if (!is_mcp && t->plugin_ref) {
                LoadedPlugin* p = (LoadedPlugin*)t->plugin_ref;
                string_append(&out, "- ");
                string_append(&out, name);
                if (p && p->name) {
                    string_append(&out, " (");
                    string_append(&out, p->name);
                    string_append(&out, ")");
                }
                string_append(&out, "\n");
            }
        }
    }

    if (mcp_count > 0) {
        string_append(&out, "\n[mcp]\n");
        for (size_t i = 0; i < total; i++) {
            Tool* t = &tool_reg->tools[i];
            const char* name = t->def.name.data ? t->def.name.data : "";
            bool is_mcp = strncmp(name, "primagen_", 9) == 0 || strncmp(name, "mcp_", 4) == 0;
            if (is_mcp) {
                string_append(&out, "- ");
                string_append(&out, name);
                string_append(&out, "\n");
            }
        }
    }

    ctx->send_response(ctx, out.data);
    string_free(&out);
    return 0;
}

static int cmd_reload_plugins(CommandContext* ctx, Config* cfg, const char* workspace_path, int argc, char** argv) {
    (void)cfg;
    (void)workspace_path;
    (void)argc;
    (void)argv;
    if (!ctx || !ctx->get_plugin_manager || !ctx->send_response) return -1;

    PluginManager* plugin_mgr = ctx->get_plugin_manager(ctx);
    if (!plugin_mgr) {
        ctx->send_response(ctx, "Plugin manager not initialized.");
        return -1;
    }

    int reloaded = plugin_manager_load_external(plugin_mgr);
    char response[512];
    snprintf(response, sizeof(response),
        "\nPlugin reload complete.\n"
        "External dir: %s\n"
        "New/reloaded plugins: %d\n"
        "Total loaded: %zu",
        plugin_mgr->external_dir, reloaded, plugin_mgr->plugin_count);
    ctx->send_response(ctx, response);
    return 0;
}

/* =============================================================================
   Built-in Command Definitions - Registered via PluginManager
   Note: Defined at runtime in agent_loop_register_builtin_commands()
   ============================================================================= */

// Command functions are defined above, array is defined at registration time

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
    loop->tool_reg = tool_reg;
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

    loop->running = false;
    loop->llm_call = NULL;
    loop->active_tasks = NULL;
    loop->current_session_key[0] = '\0';
    loop->inbox_head = NULL;
    loop->inbox_tail = NULL;
    loop->processing_thread_started = false;
    pthread_mutex_init(&loop->task_mutex, NULL);
    pthread_mutex_init(&loop->state_mutex, NULL);
    pthread_mutex_init(&loop->inbox_mutex, NULL);
    pthread_cond_init(&loop->inbox_cond, NULL);

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
    pthread_mutex_destroy(&loop->state_mutex);
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
    pthread_mutex_lock(&loop->inbox_mutex);
    pthread_mutex_lock(&loop->state_mutex);
    loop->running = false;
    pthread_mutex_unlock(&loop->state_mutex);
    message_bus_close(loop->bus);
    pthread_cond_broadcast(&loop->inbox_cond);
    pthread_mutex_unlock(&loop->inbox_mutex);
    log_debug("[AgentLoop] Stop requested");
}

/* =============================================================================
   Command Registration - Register built-in commands with PluginManager
   ============================================================================= */

void agent_loop_register_builtin_commands(AgentLoop* loop) {
    if (!loop || !loop->plugin_mgr) return;

    // Define builtin commands at runtime (consistent with builtin_tools pattern)
    struct {
        const char* name;
        const char* desc;
        CommandFunc handler;
    } builtin_commands[] = {
        {"stop", "Stop active tasks", cmd_stop},
        {"restart", "Restart the bot", cmd_restart},
        {"new", "Start a new conversation", cmd_new},
        {"help", "Show available commands", cmd_help},
        {"tools", "Show available tools (builtin/plugin/mcp)", cmd_tools},
        {"reload-plugins", "Reload all plugins from the plugins directory", cmd_reload_plugins},
    };
    #define BUILTIN_COMMAND_COUNT (sizeof(builtin_commands) / sizeof(builtin_commands[0]))

    for (size_t i = 0; i < BUILTIN_COMMAND_COUNT; i++) {
        plugin_register_command(loop->plugin_mgr, NULL,
            builtin_commands[i].name,
            builtin_commands[i].desc,
            builtin_commands[i].handler);
    }

    log_debug("[AgentLoop] Registered %zu built-in commands", BUILTIN_COMMAND_COUNT);
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
    // Define builtin tools at runtime (user_data is runtime context)
    struct {
        const char* name;
        const char* desc;
        const char* params;
        ToolExecuteFunc exec;
    } builtin_tools[] = {
        {"read_file", "Read file content",
            "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
            tool_read_file},
        {"write_file", "Write file content",
            "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}",
            tool_write_file},
        {"edit_file", "Edit file content",
            "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old_str\":{\"type\":\"string\"},\"new_str\":{\"type\":\"string\"}},\"required\":[\"path\",\"old_str\",\"new_str\"]}",
            tool_edit_file},
        {"list_dir", "List directory contents",
            "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
            tool_list_dir},
        {"exec", "Execute shell command",
            "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}",
            tool_exec},
        {"send_message", "Send message to user. Optional attachments support image/audio/video uploads.",
            "{\"type\":\"object\",\"properties\":{\"content\":{\"type\":\"string\"},\"attachments\":{\"type\":\"array\",\"items\":{\"oneOf\":[{\"type\":\"string\"},{\"type\":\"object\",\"properties\":{\"type\":{\"type\":\"string\",\"enum\":[\"image\",\"audio\",\"video\"]},\"path\":{\"type\":\"string\"},\"url\":{\"type\":\"string\"},\"duration\":{\"type\":\"integer\",\"minimum\":1},\"cover_path\":{\"type\":\"string\"}},\"required\":[\"type\",\"path\"]}]}}},\"required\":[\"content\"]}",
            tool_send_message},
        {"spawn_subagent", "Spawn subagent",
            "{\"type\":\"object\",\"properties\":{\"task\":{\"type\":\"string\"},\"label\":{\"type\":\"string\"}},\"required\":[\"task\"]}",
            tool_spawn},
        {"cron", "Schedule a reminder or recurring task. Use for future notifications. Formats: '@in N' (N seconds later, one-time), '@every N' (recurring), '@at TIMESTAMP', or 'M H * * *' (daily cron). Example: schedule a drink water reminder with name='drink-water', payload='该喝水了！', schedule='@in 1800' for 30 minutes.",
            "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Unique job identifier, e.g., 'drink-water', 'stand-up'\"},\"payload\":{\"type\":\"string\",\"description\":\"Message content to deliver when triggered\"},\"schedule\":{\"type\":\"string\",\"description\":\"When to trigger: '@in N' (N seconds), '@every N', '@at UNIX_TIMESTAMP', or 'M H * * *'\"},\"channel\":{\"type\":\"string\"},\"chat_id\":{\"type\":\"string\"}},\"required\":[\"name\",\"payload\",\"schedule\"]}",
            tool_cron},
        {"skill", "Manage skills",
            "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list\",\"load\",\"unload\"]},\"name\":{\"type\":\"string\"}},\"required\":[\"action\"]}",
            tool_skill},
        {"memory", "Manage long-term memory. Use this to consolidate conversation history into persistent memory.",
            "{\"type\":\"object\",\"properties\":{\"history_entry\":{\"type\":\"string\",\"description\":\"A paragraph summarizing key events/decisions. Start with [YYYY-MM-DD HH:MM].\"},\"memory_update\":{\"type\":\"string\",\"description\":\"Full updated long-term memory content (facts). Return unchanged if no new facts.\"}},\"required\":[\"history_entry\"]}",
            tool_memory},
    };
    #define BUILTIN_TOOLS_COUNT (sizeof(builtin_tools) / sizeof(builtin_tools[0]))

    // Register each tool with a copy of ctx as user_data
    // Each tool gets its own copy, which will be freed by tool_registry_free
    for (size_t i = 0; i < BUILTIN_TOOLS_COUNT; i++) {
        // Create a shallow copy of ToolContext for this tool
        ToolContext* tool_ctx = malloc(sizeof(ToolContext));
        if (!tool_ctx) {
            log_error("[AgentLoop] Failed to allocate ToolContext for tool: %s", builtin_tools[i].name);
            continue;
        }
        memcpy(tool_ctx, ctx, sizeof(ToolContext));
        log_debug("[AgentLoop] Created ToolContext for %s: ptr=%p, magic=0x%x, bus=%p", 
                  builtin_tools[i].name, (void*)tool_ctx, tool_ctx->magic, (void*)tool_ctx->bus);

        int result = plugin_register_tool(manager, NULL,
            builtin_tools[i].name,
            builtin_tools[i].desc,
            builtin_tools[i].params,
            builtin_tools[i].exec,
            tool_ctx);

        if (result != 0) {
            log_error("[AgentLoop] Failed to register built-in tool: %s", builtin_tools[i].name);
            free(tool_ctx);  // Registration failed, free the copy
        }
        // On success, tool_ctx ownership is transferred to tool_registry
        // and will be freed by tool_registry_free
    }

    log_debug("[AgentLoop] Registered %zu built-in tools", BUILTIN_TOOLS_COUNT);
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

static void add_active_task(AgentLoop* loop, const char* task_id, const char* session_key, pthread_t thread, InboundMessage* msg);
static void remove_active_task(AgentLoop* loop, const char* task_id);
static void process_message_async(AgentLoop* loop, InboundMessage* inbound, Session* session);
static void process_inbound_message(AgentLoop* loop, InboundMessage* inbound);
static void enqueue_inbound_task(AgentLoop* loop, InboundMessage* inbound);
static InboundMessage* dequeue_inbound_task(AgentLoop* loop);
static void* agent_loop_processing_worker(void* arg);

static void parse_slash_command(const char* full_content, char* cmd_name, size_t cmd_name_size, const char** args_start_out) {
    if (!full_content || !cmd_name || cmd_name_size == 0) {
        return;
    }

    const char* args_start = full_content[0] == '/' ? full_content + 1 : full_content;
    while (*args_start == ' ') args_start++;

    const char* space = strchr(args_start, ' ');
    if (space) {
        size_t len = (size_t)(space - args_start);
        if (len >= cmd_name_size) len = cmd_name_size - 1;
        strncpy(cmd_name, args_start, len);
        cmd_name[len] = '\0';
        args_start = space + 1;
    } else {
        strncpy(cmd_name, args_start, cmd_name_size - 1);
        cmd_name[cmd_name_size - 1] = '\0';
        args_start += strlen(args_start);
    }

    while (*args_start == ' ') args_start++;
    if (args_start_out) {
        *args_start_out = args_start;
    }
}

static bool handle_plugin_command(AgentLoop* loop, const char* channel, const char* chat_id, const char* session_key, const char* full_content) {
    if (!loop || !loop->plugin_mgr) return false;

    char cmd_name[64] = {0};
    const char* args_start = NULL;
    parse_slash_command(full_content, cmd_name, sizeof(cmd_name), &args_start);
    if (cmd_name[0] == '\0') return false;

    // Search for matching command (need lock for reading)
    pthread_mutex_lock(&loop->plugin_mgr->lock);
    CommandFunc handler = NULL;
    for (size_t i = 0; i < loop->plugin_mgr->command_count; i++) {
        if (strcmp(loop->plugin_mgr->commands[i].name, cmd_name) == 0) {
            handler = loop->plugin_mgr->commands[i].handler;
            break;
        }
    }
    pthread_mutex_unlock(&loop->plugin_mgr->lock);

    // Execute handler outside lock to avoid deadlock
    // (handler may need to access plugin_mgr internally)
    if (handler) {
        log_debug("[AgentLoop] Executing command: /%s", cmd_name);

        CommandRuntimeData runtime_data = {
            .loop = loop,
            .session_key = session_key,
            .channel = channel,
            .chat_id = chat_id
        };
        CommandContext cmd_ctx = {
            .user_data = &runtime_data,
            .send_response = command_ctx_send_response,
            .stop_active_tasks = command_ctx_stop_active_tasks,
            .reset_session = command_ctx_reset_session,
            .get_registered_commands = command_ctx_get_registered_commands,
            .get_tool_registry = command_ctx_get_tool_registry,
            .get_plugin_manager = command_ctx_get_plugin_manager
        };

        char* argv[16] = {0};
        int argc = 0;

        char* args_copy = strdup(args_start ? args_start : "");
        if (args_copy) {
            char* token = strtok(args_copy, " ");
            while (token && argc < 16) {
                argv[argc++] = token;
                token = strtok(NULL, " ");
            }
        }

        int result = handler(&cmd_ctx, loop->config, loop->workspace_path, argc, argv);

        log_debug("[AgentLoop] Command /%s executed with result: %d", cmd_name, result);
        free(args_copy);
        return result >= 0;
    }

    return false;
}

static bool build_tool_args_json(const char* tool_name, const char* args_start, String* out_json) {
    if (!tool_name || !out_json) return false;
    const char* args = args_start ? args_start : "";
    while (*args == ' ') args++;
    if (args[0] == '\0') {
        *out_json = string_new("{}");
        return true;
    }
    if (args[0] == '{') {
        *out_json = string_new(args);
        return true;
    }

    const char* key = NULL;
    if (strcmp(tool_name, "web_fetch") == 0) key = "url";
    else if (strcmp(tool_name, "web_search") == 0) key = "query";
    else if (strcmp(tool_name, "exec") == 0) key = "command";
    else if (strcmp(tool_name, "read_file") == 0 || strcmp(tool_name, "list_dir") == 0) key = "path";
    if (!key) return false;

    cJSON* root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, key, args);
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return false;
    *out_json = string_new(text);
    free(text);
    return true;
}

static bool handle_tool_fallback_command(AgentLoop* loop, InboundMessage* inbound, const char* full_content) {
    if (!loop || !loop->tool_reg || !inbound || !full_content) return false;

    char tool_name[64] = {0};
    const char* args_start = NULL;
    parse_slash_command(full_content, tool_name, sizeof(tool_name), &args_start);
    if (tool_name[0] == '\0') return false;

    Tool* tool = tool_registry_get(loop->tool_reg, tool_name);
    if (!tool) return false;

    String args_json = {0};
    if (!build_tool_args_json(tool_name, args_start, &args_json)) return false;

    String result = string_new("");
    Error exec_err = tool_registry_execute(loop->tool_reg, tool_name, args_json.data, &result);
    string_free(&args_json);

    if (exec_err.code != ERR_NONE) {
        char response[512];
        snprintf(response, sizeof(response), "Tool '%s' execution failed: %s", tool_name, exec_err.message);
        OutboundMessage* outbound = outbound_message_new(inbound->channel.data, inbound->chat_id.data, response);
        message_bus_send_outbound(loop->bus, outbound);
        string_free(&result);
        return true;
    }

    const char* response_text = (result.data && result.data[0] != '\0') ? result.data : "Tool executed successfully.";
    OutboundMessage* outbound = outbound_message_new(inbound->channel.data, inbound->chat_id.data, response_text);
    message_bus_send_outbound(loop->bus, outbound);
    string_free(&result);
    return true;
}

static bool handle_skill_fallback_command(AgentLoop* loop, InboundMessage* inbound, Session* session, const char* session_key, const char* full_content) {
    if (!loop || !inbound || !session || !session_key || !full_content) return false;

    char skill_name[64] = {0};
    const char* args_start = NULL;
    parse_slash_command(full_content, skill_name, sizeof(skill_name), &args_start);
    if (skill_name[0] == '\0') return false;

    Tool* skill_tool = tool_registry_get(loop->tool_reg, "skill");
    if (!skill_tool || !skill_tool->user_data) return false;

    ToolContext* tool_ctx = (ToolContext*)skill_tool->user_data;
    if (!tool_ctx->skills_loader) return false;

    char* loaded_skill = skills_loader_load_skill(tool_ctx->skills_loader, skill_name);
    if (!loaded_skill) return false;
    free(loaded_skill);

    String trigger = string_new("");
    string_append(&trigger, "Run skill '");
    string_append(&trigger, skill_name);
    string_append(&trigger, "' for this request.");
    if (args_start && *args_start) {
        string_append(&trigger, " User input: ");
        string_append(&trigger, args_start);
    }

    Message* user_msg = message_new(ROLE_USER, trigger.data);
    session_add_message(session, user_msg);
    string_free(&trigger);

    char task_id[32];
    snprintf(task_id, sizeof(task_id), "task_%ld", time(NULL));
    pthread_t current_thread = pthread_self();
    add_active_task(loop, task_id, session_key, current_thread, inbound);
    process_message_async(loop, inbound, session);
    // remove_active_task is now handled asynchronously when the task finishes
    
    log_info("[AgentLoop] Fallback slash command '/%s' matched skill and executed", skill_name);
    return true;
}

/* =============================================================================
   Task Tracking
   ============================================================================= */

static void add_active_task(AgentLoop* loop, const char* task_id, const char* session_key, pthread_t thread, InboundMessage* msg) {
    pthread_mutex_lock(&loop->task_mutex);

    ActiveTaskNode* node = malloc(sizeof(ActiveTaskNode));
    if (node) {
        strncpy(node->task_id, task_id, sizeof(node->task_id) - 1);
        node->task_id[sizeof(node->task_id) - 1] = '\0';
        strncpy(node->session_key, session_key, sizeof(node->session_key) - 1);
        node->session_key[sizeof(node->session_key) - 1] = '\0';
        node->thread = thread;
        node->cancelling = false;
        
        node->ctx.state = SESSION_STATE_IDLE;
        node->ctx.turn = 0;
        if (msg) {
            strncpy(node->ctx.channel, msg->channel.data, sizeof(node->ctx.channel) - 1);
            strncpy(node->ctx.chat_id, msg->chat_id.data, sizeof(node->ctx.chat_id) - 1);
            strncpy(node->ctx.latest_user_content, msg->content.data, sizeof(node->ctx.latest_user_content) - 1);
            node->ctx.channel[sizeof(node->ctx.channel)-1] = '\0';
            node->ctx.chat_id[sizeof(node->ctx.chat_id)-1] = '\0';
            node->ctx.latest_user_content[sizeof(node->ctx.latest_user_content)-1] = '\0';
        } else {
            node->ctx.channel[0] = '\0';
            node->ctx.chat_id[0] = '\0';
            node->ctx.latest_user_content[0] = '\0';
        }

        node->next = loop->active_tasks;
        loop->active_tasks = node;
        log_debug("[AgentLoop] Added active task: %s", task_id);
    }

    pthread_mutex_unlock(&loop->task_mutex);
}

static ActiveTaskNode* get_active_task(AgentLoop* loop, const char* session_key) {
    pthread_mutex_lock(&loop->task_mutex);
    ActiveTaskNode* current = loop->active_tasks;
    while (current) {
        if (strcmp(current->session_key, session_key) == 0) {
            pthread_mutex_unlock(&loop->task_mutex);
            return current;
        }
        current = current->next;
    }
    pthread_mutex_unlock(&loop->task_mutex);
    return NULL;
}

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

static void refresh_tool_routes(AgentLoop* loop, const char* channel, const char* chat_id) {
    log_debug("[AgentLoop] refresh_tool_routes: channel=%s, chat_id=%s", channel, chat_id);
    const char* tool_names[] = {"cron", "send_message", "spawn_subagent", "skill", "memory", "exec"};
    size_t count = sizeof(tool_names) / sizeof(tool_names[0]);
    for (size_t i = 0; i < count; i++) {
        Tool* tool = tool_registry_get(loop->tool_reg, tool_names[i]);
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

static void trigger_llm_async(AgentLoop* loop, Session* session, const char* session_key, const char* channel, const char* chat_id) {
    String system_prompt = context_builder_build_with_channel(
        loop->ctx_builder, session, loop->tool_reg, channel, chat_id
    );

    AsyncContext* ctx = malloc(sizeof(AsyncContext));
    ctx->loop = loop;
    strncpy(ctx->session_key, session_key, sizeof(ctx->session_key) - 1);
    ctx->session_key[sizeof(ctx->session_key) - 1] = '\0';

    if (loop->llm_call_async) {
        loop->llm_call_async(system_prompt.data, session, loop->tool_reg, loop->config, handle_llm_callback, ctx);
    } else {
        log_error("[AgentLoop] No async LLM provider configured");
        handle_llm_callback(error_new(ERR_INVALID_PARAM, "No async LLM provider configured"), NULL, NULL, 0, ctx);
    }

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
    ActiveTaskNode* task = get_active_task(loop, event->session_key.data);
    if (!task) return;
    
    Session* session = session_manager_get(loop->session_mgr, event->session_key.data);
    if (!session) return;
    
    if (event->llm_error.code != ERR_NONE) {
        log_error("[AgentLoop] LLM call error: %s", event->llm_error.message);
        char full_msg[512];
        snprintf(full_msg, sizeof(full_msg), "Sorry, I encountered an error: %s", event->llm_error.message);
        Message* assistant_msg = message_new(ROLE_ASSISTANT, full_msg);
        session_add_message(session, assistant_msg);
        session_manager_save(loop->session_mgr, session);
        OutboundMessage* outbound = outbound_message_new(task->ctx.channel, task->ctx.chat_id, full_msg);
        message_bus_send_outbound(loop->bus, outbound);
        
        remove_active_task(loop, task->task_id);
        return;
    }

    char* clean_content = strip_think_tags(event->llm_response.data);

    if (event->tool_calls_count == 0) {
        const char* assistant_content = (clean_content && strlen(clean_content) > 0) ? clean_content : event->llm_response.data;
        if (assistant_content && strlen(assistant_content) > 0) {
            Message* assistant_msg = message_new(ROLE_ASSISTANT, assistant_content);
            session_add_message(session, assistant_msg);
            session_manager_save(loop->session_mgr, session);
        }
        
        // Use clean_content if available, otherwise response.data. If both null/empty, use ""
    const char* final_out = (assistant_content && strlen(assistant_content) > 0) ? assistant_content : "";
    if (final_out && strlen(final_out) > 0) {
        OutboundMessage* outbound = outbound_message_new(task->ctx.channel, task->ctx.chat_id, final_out);
        message_bus_send_outbound(loop->bus, outbound);
    } else {
        // Send a specific internal marker or empty message so channel knows it's completed
        OutboundMessage* outbound = outbound_message_new(task->ctx.channel, task->ctx.chat_id, "");
        message_bus_send_outbound(loop->bus, outbound);
    }
        
        if (clean_content) free(clean_content);
        
        remove_active_task(loop, task->task_id);
    } else {
        // Rewrite missing tools to skill load
        for (size_t i = 0; i < event->tool_calls_count; i++) {
            if (tool_registry_get(loop->tool_reg, event->tool_calls[i].name.data) == NULL &&
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

        Message* assistant_msg = message_new(ROLE_ASSISTANT, clean_content ? clean_content : event->llm_response.data);
        for (size_t i = 0; i < event->tool_calls_count; i++) {
            message_add_tool_call(assistant_msg, event->tool_calls[i].id.data, event->tool_calls[i].name.data, event->tool_calls[i].arguments.data);
        }
        session_add_message(session, assistant_msg);
        session_manager_save(loop->session_mgr, session);

        if (clean_content && clean_content != event->llm_response.data) free(clean_content);

        // For pending tools, trigger next step
        task->ctx.state = SESSION_STATE_WAITING_TOOL;
        
        // Execute tools asynchronously
        // Note: Currently we only support one async tool execution per event well, 
        // to handle multiple parallel tools we need a join mechanism or queue.
        // For simplicity, if multiple tools are returned, we submit them sequentially.
        // The state machine needs to track pending tools if > 1.
        // For now, we assume sequential or we just submit the first one to test.
        // Let's iterate and submit, but handle_event_tool_result needs to know when ALL are done.
        // Since this is a refactor, let's execute sequentially for now by submitting one, 
        // and keeping the rest in state, OR submit all and count responses.
        // Let's use `tool_executor_execute_sync` inside a wrapper or modify ToolExecutor to be async.
        // Actually, ToolExecutor is already thread-pool based (`tool_executor_submit`).
        // We will just submit all and handle them as they come. But we must know when the turn is done.
        // Easiest is to execute sync here in a separate thread, but that defeats the purpose.
        // Let's do the simplest async conversion: execute the first tool, when it returns, execute next, etc.
        // Or execute all, and wait for N results.
        
        // We will execute sync for now BUT wrapped in an async task submission to avoid blocking AgentLoop thread.
        // Since tool_executor_submit takes a task, we can pass a callback.
        
        // As a temporary bridge for Phase 2:
        // IMPORTANT: Update ToolContext with the correct channel/chat_id before executing tools
        // This ensures tools send messages to the correct destination
        refresh_tool_routes(loop, task->ctx.channel, task->ctx.chat_id);
        
        for (size_t i = 0; i < event->tool_calls_count; i++) {
            ToolAsyncContext* tctx = malloc(sizeof(ToolAsyncContext));
            tctx->loop = loop;
            strncpy(tctx->session_key, event->session_key.data, sizeof(tctx->session_key) - 1);
            tctx->session_key[sizeof(tctx->session_key) - 1] = '\0';
            strncpy(tctx->tool_call_id, event->tool_calls[i].id.data, sizeof(tctx->tool_call_id) - 1);
            tctx->tool_call_id[sizeof(tctx->tool_call_id) - 1] = '\0';
            strncpy(tctx->tool_name, event->tool_calls[i].name.data, sizeof(tctx->tool_name) - 1);
            tctx->tool_name[sizeof(tctx->tool_name) - 1] = '\0';
            
            // Submitting to thread pool
            tool_executor_submit_async(loop->tool_executor, event->tool_calls[i].name.data, event->tool_calls[i].arguments.data, tool_executor_callback, tctx);
        }
    }
}

static void handle_event_tool_result(AgentLoop* loop, InternalEvent* event) {
    ActiveTaskNode* task = get_active_task(loop, event->session_key.data);
    if (!task) return;

    Session* session = session_manager_get(loop->session_mgr, event->session_key.data);
    if (!session) return;

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

    // Check if all tools for this turn are done (simplified: we assume 1 tool or we trigger LLM immediately, which might cause issues if multiple tools. In a full implementation, we need a pending_tools counter).
    // For now, trigger LLM immediately. If multiple tools, this will trigger multiple LLM calls. We should add a counter.
    // Let's add a quick hack: we only trigger LLM if no other tools are pending.
    // For now, let's just trigger it.
    
    int max_turns = loop->config && loop->config->agent.max_tool_iterations > 0 ? loop->config->agent.max_tool_iterations : 15;
    if (task->ctx.turn >= max_turns) {
        log_warn("[AgentLoop] Max iterations (%d) reached", max_turns);
        send_error_response(loop, task->ctx.channel, task->ctx.chat_id, "I reached the maximum number of tool call iterations without completing the task.");
        remove_active_task(loop, task->task_id);
        return;
    }

    task->ctx.turn++;
    task->ctx.state = SESSION_STATE_WAITING_LLM;
    trigger_llm_async(loop, session, event->session_key.data, task->ctx.channel, task->ctx.chat_id);
}

static void process_message_async(AgentLoop* loop, InboundMessage* inbound, Session* session) {
    char key[256];
    snprintf(key, sizeof(key), "%s:%s", inbound->channel.data, inbound->chat_id.data);

    strncpy(loop->current_session_key, key, sizeof(loop->current_session_key) - 1);
    loop->current_session_key[sizeof(loop->current_session_key) - 1] = '\0';

    refresh_tool_routes(loop, inbound->channel.data, inbound->chat_id.data);

    ActiveTaskNode* task = get_active_task(loop, key);
    if (!task) return;

    if (task->ctx.state != SESSION_STATE_IDLE) {
        log_warn("[AgentLoop] Session %s is busy", key);
        return;
    }

    log_debug("[AgentLoop] Processing message for session: %s", key);

    task->ctx.state = SESSION_STATE_WAITING_LLM;
    task->ctx.turn = 1;
    trigger_llm_async(loop, session, key, inbound->channel.data, inbound->chat_id.data);
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
    if (content[0] == '/') {
        bool handled = handle_plugin_command(loop, inbound->channel.data, inbound->chat_id.data, key, content);
        if (!handled) {
            handled = handle_tool_fallback_command(loop, inbound, content);
        }
        if (!handled) {
            handled = handle_skill_fallback_command(loop, inbound, session, key, content);
        }
        if (!handled) {
            char response[256];
            snprintf(response, sizeof(response), "Unknown command: %s. Type /help for available commands.", content);
            OutboundMessage* outbound = outbound_message_new(inbound->channel.data, inbound->chat_id.data, response);
            message_bus_send_outbound(loop->bus, outbound);
        }
        return;
    }

    Message* user_msg = message_new(ROLE_USER, inbound->content.data);
    session_add_message(session, user_msg);
    maybe_auto_consolidate_memory(loop, session, key, inbound->content.data);

    char task_id[32];
    snprintf(task_id, sizeof(task_id), "task_%ld", time(NULL));
    pthread_t current_thread = pthread_self();
    add_active_task(loop, task_id, key, current_thread, inbound);
    process_message_async(loop, inbound, session);
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

        if (strcmp(inbound->channel.data, "system") == 0 && strcmp(inbound->content.data, "exit") == 0) {
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
