#include "agent_loop.h"
#include "../include/common.h"
#include "../include/logger.h"
#include "../tools/tools_impl.h"
#include "../tools/tool_executor.h"
#include "../include/utils.h"
#include "../plugin/plugin_manager.h"
#include <string.h>
#include <stdio.h>

/* Forward declaration - defined in utils.c */
extern char* strip_think_tags(const char* text);

/* =============================================================================
   Command Parameter Macros - Standard argv layout for all commands
   ============================================================================= */

// Standard argv layout: [loop, session_key, channel, chat_id, bus, ...user_args]
#define CMD_ARG_LOOP(argv)         ((AgentLoop*)(argv)[0])
#define CMD_ARG_SESSION_KEY(argv)  ((const char*)(argv)[1])
#define CMD_ARG_CHANNEL(argv)      ((const char*)(argv)[2])
#define CMD_ARG_CHAT_ID(argv)      ((const char*)(argv)[3])
#define CMD_ARG_BUS(argv)          ((MessageBus*)(argv)[4])
#define CMD_ARG_MIN_COUNT          (5)  // Minimum argc for commands

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

/* =============================================================================
   Built-in Commands - Follow CommandFunc signature exactly
   Context passed via argv: [loop, session_key, channel, chat_id, bus, ...user_args]
   ============================================================================= */

static int cmd_stop(Config* cfg, const char* workspace_path, int argc, char** argv) {
    (void)cfg; (void)workspace_path;
    if (argc < CMD_ARG_MIN_COUNT) return -1;

    AgentLoop* loop = CMD_ARG_LOOP(argv);
    const char* session_key = CMD_ARG_SESSION_KEY(argv);
    const char* channel = CMD_ARG_CHANNEL(argv);
    const char* chat_id = CMD_ARG_CHAT_ID(argv);
    MessageBus* bus = CMD_ARG_BUS(argv);

    if (!loop || !session_key) return 0;

    int cancelled = 0;
    pthread_mutex_lock(&loop->task_mutex);
    ActiveTaskNode* current = loop->active_tasks;
    while (current) {
        if (strcmp(current->session_key, session_key) == 0 && !current->cancelling) {
            current->cancelling = true;
            cancelled++;
        }
        current = current->next;
    }
    pthread_mutex_unlock(&loop->task_mutex);

    char response[256];
    snprintf(response, sizeof(response), cancelled > 0 ? "Stopped %d task(s)." : "No active task to stop.", cancelled);
    OutboundMessage* outbound = outbound_message_new(channel, chat_id, response);
    message_bus_send_outbound(bus, outbound);
    return cancelled;
}

static int cmd_restart(Config* cfg, const char* workspace_path, int argc, char** argv) {
    (void)cfg; (void)workspace_path;
    if (argc < CMD_ARG_MIN_COUNT) return -1;

    AgentLoop* loop = CMD_ARG_LOOP(argv);
    const char* channel = CMD_ARG_CHANNEL(argv);
    const char* chat_id = CMD_ARG_CHAT_ID(argv);

    OutboundMessage* outbound = outbound_message_new(channel, chat_id, "Restarting...");
    message_bus_send_outbound(loop->bus, outbound);
    log_debug("[AgentLoop] Restart requested but requires external wrapper");
    return 0;
}

static int cmd_new(Config* cfg, const char* workspace_path, int argc, char** argv) {
    (void)cfg; (void)workspace_path;
    if (argc < CMD_ARG_MIN_COUNT) return -1;

    AgentLoop* loop = CMD_ARG_LOOP(argv);
    const char* session_key = CMD_ARG_SESSION_KEY(argv);
    const char* channel = CMD_ARG_CHANNEL(argv);
    const char* chat_id = CMD_ARG_CHAT_ID(argv);

    Session* session = session_manager_get(loop->session_mgr, session_key);
    if (!session) session_manager_load(loop->session_mgr, session_key, &session);
    if (!session) return -1;

    for (size_t i = 0; i < session->messages.count; i++) {
        Message* msg = *(Message**)dynamic_array_get(&session->messages, i);
        message_free(msg);
    }
    dynamic_array_clear_impl(&session->messages);
    session->last_consolidated = 0;
    session->updated_at = time(NULL);
    session_manager_save(loop->session_mgr, session);

    OutboundMessage* outbound = outbound_message_new(channel, chat_id, "New session started.");
    message_bus_send_outbound(loop->bus, outbound);
    return 0;
}

static int cmd_help(Config* cfg, const char* workspace_path, int argc, char** argv) {
    (void)cfg; (void)workspace_path;
    if (argc < CMD_ARG_MIN_COUNT) return -1;

    AgentLoop* loop = CMD_ARG_LOOP(argv);
    const char* channel = CMD_ARG_CHANNEL(argv);
    const char* chat_id = CMD_ARG_CHAT_ID(argv);
    MessageBus* bus = CMD_ARG_BUS(argv);

    // Get all registered commands dynamically from PluginManager
    size_t command_count = 0;
    CommandPluginDef* commands = plugin_manager_get_commands(loop->plugin_mgr, &command_count);

    // Build help text dynamically
    String help_text = string_new("");
    string_append(&help_text, "Primagen commands:\n");

    if (commands && command_count > 0) {
        for (size_t i = 0; i < command_count; i++) {
            string_append(&help_text, "/");
            string_append(&help_text, commands[i].name);
            string_append(&help_text, " - ");
            string_append(&help_text, commands[i].description);
            if (i < command_count - 1) {
                string_append(&help_text, "\n");
            }
        }
    } else {
        string_append(&help_text, "No commands registered.");
    }

    OutboundMessage* outbound = outbound_message_new(channel, chat_id, help_text.data);
    message_bus_send_outbound(bus, outbound);

    string_free(&help_text);
    return 0;
}

static int cmd_reload_plugins(Config* cfg, const char* workspace_path, int argc, char** argv) {
    (void)cfg; (void)workspace_path;
    if (argc < CMD_ARG_MIN_COUNT) return -1;

    AgentLoop* loop = CMD_ARG_LOOP(argv);
    const char* channel = CMD_ARG_CHANNEL(argv);
    const char* chat_id = CMD_ARG_CHAT_ID(argv);

    if (!loop || !loop->plugin_mgr) {
        OutboundMessage* outbound = outbound_message_new(channel, chat_id, "Plugin manager not initialized.");
        message_bus_send_outbound(loop->bus, outbound);
        return -1;
    }

    int reloaded = plugin_manager_load_external(loop->plugin_mgr);
    char response[512];
    snprintf(response, sizeof(response),
        "Plugin reload complete.\n"
        "External dir: %s\n"
        "New/reloaded plugins: %d\n"
        "Total loaded: %zu",
        loop->plugin_mgr->external_dir, reloaded, loop->plugin_mgr->plugin_count);

    OutboundMessage* outbound = outbound_message_new(channel, chat_id, response);
    message_bus_send_outbound(loop->bus, outbound);
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
extern Error tool_web_search(void* user_data, const char* args_json, String* result);
extern Error tool_web_fetch(void* user_data, const char* args_json, String* result);
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
    pthread_mutex_init(&loop->task_mutex, NULL);

    log_debug("[AgentLoop] Created new instance");
    return loop;
}

void agent_loop_free(AgentLoop* loop) {
    if (!loop) return;

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
    log_debug("[AgentLoop] Freed instance");
    free(loop);
}

void agent_loop_set_llm_provider(AgentLoop* loop, LLMProvider provider) {
    if (!loop) return;
    loop->llm_call = provider;
    log_debug("[AgentLoop] LLM provider set");
}

void agent_loop_stop(AgentLoop* loop) {
    if (!loop) return;
    loop->running = false;
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
        {"web_search", "Search the web",
            "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"count\":{\"type\":\"integer\"}},\"required\":[\"query\"]}",
            tool_web_search},
        {"web_fetch", "Fetch URL content",
            "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}",
            tool_web_fetch},
        {"send_message", "Send message to user",
            "{\"type\":\"object\",\"properties\":{\"content\":{\"type\":\"string\"}},\"required\":[\"content\"]}",
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

static bool handle_plugin_command(AgentLoop* loop, const char* channel, const char* chat_id, const char* session_key, const char* full_content) {
    if (!loop || !loop->plugin_mgr) return false;

    // Extract command name (first word after /)
    char cmd_name[64];
    const char* args_start = full_content + 1;
    while (*args_start == ' ') args_start++;

    const char* space = strchr(args_start, ' ');
    if (space) {
        size_t len = space - args_start;
        if (len >= sizeof(cmd_name)) len = sizeof(cmd_name) - 1;
        strncpy(cmd_name, args_start, len);
        cmd_name[len] = '\0';
        args_start = space + 1;
    } else {
        strncpy(cmd_name, args_start, sizeof(cmd_name) - 1);
        cmd_name[sizeof(cmd_name) - 1] = '\0';
    }

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

        // Build argv with context: [loop, session_key, channel, chat_id, bus, ...user_args]
        char* argv[16] = {0};
        int argc = 0;

        argv[argc++] = (char*)loop;
        argv[argc++] = (char*)session_key;
        argv[argc++] = (char*)channel;
        argv[argc++] = (char*)chat_id;
        argv[argc++] = (char*)loop->bus;

        char* args_copy = strdup(args_start);
        char* token = strtok(args_copy, " ");
        while (token && argc < 15) {
            argv[argc++] = token;
            token = strtok(NULL, " ");
        }

        int result = handler(loop->config, loop->workspace_path, argc, argv);

        log_debug("[AgentLoop] Command /%s executed with result: %d", cmd_name, result);
        free(args_copy);
        return result >= 0;
    }

    return false;
}

/* =============================================================================
   Task Tracking
   ============================================================================= */

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

/* =============================================================================
   Message Processing
   ============================================================================= */

static Error process_message(AgentLoop* loop, InboundMessage* inbound, Session* session) {
    char key[256];
    snprintf(key, sizeof(key), "%s:%s", inbound->channel.data, inbound->chat_id.data);

    strncpy(loop->current_session_key, key, sizeof(loop->current_session_key) - 1);

    /* Set tool context */
    Tool* cron_tool = tool_registry_get(loop->tool_reg, "cron");
    if (cron_tool && cron_tool->user_data) {
        tool_context_set_route((ToolContext*)cron_tool->user_data,
                               inbound->channel.data, inbound->chat_id.data);
    }

    // Get max_turns from config, default to 15 if not set
    int max_turns = loop->config && loop->config->agent.max_tool_iterations > 0
                    ? loop->config->agent.max_tool_iterations : 15;
    int turn = 0;
    bool conversation_turn_done = false;
    bool error_occurred = false;

    log_debug("[AgentLoop] Processing message for session: %s", key);

    while (!conversation_turn_done && turn < max_turns && loop->running) {
        turn++;
        log_debug("[AgentLoop] Turn %d/%d", turn, max_turns);

        String system_prompt = context_builder_build_with_channel(
            loop->ctx_builder, session, loop->tool_reg,
            inbound->channel.data, inbound->chat_id.data
        );

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

        char* clean_content = strip_think_tags(response.data);

        if (tool_calls_count == 0) {
            if (!error_occurred && clean_content && strlen(clean_content) > 0) {
                OutboundMessage* outbound = outbound_message_new(inbound->channel.data, inbound->chat_id.data, clean_content);
                message_bus_send_outbound(loop->bus, outbound);
            }
            if (clean_content) free(clean_content);
            string_free(&response);
            conversation_turn_done = true;
        } else {
            Message* assistant_msg = message_new(ROLE_ASSISTANT, clean_content ? clean_content : response.data);
            for (size_t i = 0; i < tool_calls_count; i++) {
                message_add_tool_call(assistant_msg, tool_calls[i].id.data, tool_calls[i].name.data, tool_calls[i].arguments.data);
            }
            session_add_message(session, assistant_msg);

            if (clean_content && clean_content != response.data) {
                free(clean_content);
            }

            for (size_t i = 0; i < tool_calls_count; i++) {
                String result = string_new("");
                log_debug("[AgentLoop] Executing tool: %s", tool_calls[i].name.data);

                err = tool_executor_execute_sync(loop->tool_executor, tool_calls[i].name.data,
                                                  tool_calls[i].arguments.data, &result, 30000);
                if (err.code != ERR_NONE) {
                    log_error("[AgentLoop] Tool Execution Failed: %s", err.message);
                    string_free(&result);
                    if (strstr(err.message, "not found") != NULL || strstr(err.message, "Unknown tool") != NULL) {
                        char hint[512];
                        snprintf(hint, sizeof(hint),
                            "Error: '%s' is not a registered tool. "
                            "If this is a skill, use the `skill` tool first: {\"action\": \"load\", \"name\": \"%s\"}.",
                            tool_calls[i].name.data, tool_calls[i].name.data);
                        result = string_new(hint);
                        log_debug("[AgentLoop] '%s' may be a skill - suggested: use `skill` tool", tool_calls[i].name.data);
                    } else {
                        result = string_new(err.message);
                    }
                } else {
                    if (strcmp(tool_calls[i].name.data, "skill") == 0) {
                        log_debug("[AgentLoop] Tool Result: [Skill content loaded, length: %zu bytes]", result.len);
                    } else {
                        log_debug("[AgentLoop] Tool Result: %s", result.data);
                    }
                }

                Message* tool_msg = message_new(ROLE_TOOL, result.data);
                tool_msg->tool_call_id = string_copy(&tool_calls[i].id);
                tool_msg->name = string_copy(&tool_calls[i].name);
                session_add_message(session, tool_msg);
                string_free(&result);
            }

            string_free(&response);

            for (size_t i = 0; i < tool_calls_count; i++) {
                string_free(&tool_calls[i].id);
                string_free(&tool_calls[i].name);
                string_free(&tool_calls[i].arguments);
            }
            free(tool_calls);

            session_manager_save(loop->session_mgr, session);
        }
    }

    if (turn >= max_turns && !conversation_turn_done) {
        log_warn("[AgentLoop] Max iterations (%d) reached", max_turns);
        send_error_response(loop, inbound->channel.data, inbound->chat_id.data,
            "I reached the maximum number of tool call iterations without completing the task.");
    }

    if (!error_occurred) {
        session_manager_save(loop->session_mgr, session);
    }

    loop->current_session_key[0] = '\0';

    return error_new(ERR_NONE, "");
}

/* =============================================================================
   Main Agent Loop - Unified command handling
   ============================================================================= */

void agent_loop_run(AgentLoop* loop) {
    loop->running = true;
    log_debug("[AgentLoop] Started");

    while (loop->running) {
        InboundMessage* inbound = message_bus_receive_inbound(loop->bus);
        if (!inbound) {
            usleep(100000);
            continue;
        }

        if (strcmp(inbound->channel.data, "system") == 0 && strcmp(inbound->content.data, "exit") == 0) {
            log_debug("[AgentLoop] System exit received, shutting down");
            loop->running = false;
            inbound_message_free(inbound);
            break;
        }

        if (inbound->content.len == 0) {
            inbound_message_free(inbound);
            continue;
        }

        char key[256];
        snprintf(key, sizeof(key), "%s:%s", inbound->channel.data, inbound->chat_id.data);
        Session* session = session_manager_get(loop->session_mgr, key);
        if (!session) {
            session_manager_load(loop->session_mgr, key, &session);
            log_debug("[AgentLoop] Loaded session: %s", key);
        }

        /* Unified slash command handling - all commands go through plugin system */
        const char* content = inbound->content.data;
        if (content[0] == '/') {
            if (!handle_plugin_command(loop, inbound->channel.data, inbound->chat_id.data, key, content)) {
                char response[256];
                snprintf(response, sizeof(response), "Unknown command: %s. Type /help for available commands.", content);
                OutboundMessage* outbound = outbound_message_new(inbound->channel.data, inbound->chat_id.data, response);
                message_bus_send_outbound(loop->bus, outbound);
            }
            inbound_message_free(inbound);
            continue;
        }

        Message* user_msg = message_new(ROLE_USER, inbound->content.data);
        session_add_message(session, user_msg);

        char task_id[32];
        snprintf(task_id, sizeof(task_id), "task_%ld", time(NULL));
        pthread_t current_thread = pthread_self();
        add_active_task(loop, task_id, key, current_thread);

        process_message(loop, inbound, session);

        remove_active_task(loop, task_id);
        inbound_message_free(inbound);
    }

    log_debug("[AgentLoop] Stopped");
}
