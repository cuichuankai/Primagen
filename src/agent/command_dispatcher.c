#include "command_dispatcher.h"
#include "agent_common.h"
#include "agent_loop.h"
#include "tool_router.h"
#include "session_orchestrator.h"
#include "include/logger.h"
#include "include/utils.h"
#include "include/skills.h"
#include "plugin/plugin_manager.h"
#include "../context/context_builder.h"
#include "../memory/memory.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

/* add_active_task is declared in agent_loop.h */
extern void refresh_tool_routes(AgentLoop* loop, const char* channel, const char* chat_id);
extern void trigger_llm_async(AgentLoop* loop, Session* session, const char* session_key, const char* channel, const char* chat_id);

// Command context methods
static int command_ctx_send_response(CommandContext* ctx, const char* message) {
    if (!ctx || !ctx->user_data) return -1;
    
    // User data contains the necessary information
    void** user_data = (void**)ctx->user_data;
    AgentLoop* loop = (AgentLoop*)user_data[0];
    const char* channel = (const char*)user_data[1];
    const char* chat_id = (const char*)user_data[2];
    
    // Create outbound message
    OutboundMessage* outbound = outbound_message_new(
        channel, 
        chat_id, 
        message
    );
    if (!outbound) return -1;
    
    // Send through message bus
    message_bus_send_outbound(loop->bus, outbound);
    return 0;
}

static int command_ctx_stop_active_tasks(CommandContext* ctx) {
    if (!ctx || !ctx->user_data) return -1;
    
    void** user_data = (void**)ctx->user_data;
    AgentLoop* loop = (AgentLoop*)user_data[0];
    const char* session_key = (const char*)user_data[3];
    
    if (loop->session_orchestrator) {
        if (session_key && session_key[0] != '\0') {
            return session_orchestrator_cancel_tasks_by_session(loop->session_orchestrator, session_key);
        }
        session_orchestrator_cancel_all_tasks(loop->session_orchestrator);
    }
    return 0;
}

static int command_ctx_reset_session(CommandContext* ctx) {
    if (!ctx || !ctx->user_data) return -1;
    
    // User data contains the necessary information
    void** user_data = (void**)ctx->user_data;
    AgentLoop* loop = (AgentLoop*)user_data[0];
    const char* session_key = (const char*)user_data[3];
    
    // Reset session using SessionOrchestrator
    if (loop->session_orchestrator) {
        log_debug("[CommandDispatcher] Resetting session: %s", session_key);
    }
    return 0;
}

static CommandPluginDef* command_ctx_get_registered_commands(CommandContext* ctx, size_t* out_count) {
    if (!ctx || !ctx->user_data || !out_count) return NULL;
    
    // User data contains the necessary information
    void** user_data = (void**)ctx->user_data;
    AgentLoop* loop = (AgentLoop*)user_data[0];
    
    // Get commands from plugin manager
    if (loop->plugin_mgr) {
        *out_count = loop->plugin_mgr->command_count;
        return loop->plugin_mgr->commands;
    }
    *out_count = 0;
    return NULL;
}

static ToolRegistry* command_ctx_get_tool_registry(CommandContext* ctx) {
    if (!ctx || !ctx->user_data) return NULL;
    
    // User data contains the necessary information
    void** user_data = (void**)ctx->user_data;
    AgentLoop* loop = (AgentLoop*)user_data[0];
    
    return loop->tool_registry;
}

static struct PluginManager* command_ctx_get_plugin_manager(CommandContext* ctx) {
    if (!ctx || !ctx->user_data) return NULL;
    
    // User data contains the necessary information
    void** user_data = (void**)ctx->user_data;
    AgentLoop* loop = (AgentLoop*)user_data[0];
    
    return loop->plugin_mgr;
}

// Help command handler
static int cmd_help(CommandContext* ctx, Config* cfg, const char* workspace_path, int argc, char** argv) {
    (void)cfg;
    (void)workspace_path;
    (void)argc;
    (void)argv;
    if (!ctx || !ctx->get_registered_commands || !ctx->send_response) return -1;
    
    String help_text = string_new("");
    
    size_t command_count = 0;
    CommandPluginDef* commands = ctx->get_registered_commands(ctx, &command_count);
    
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

// Stop command handler
static int cmd_stop(CommandContext* ctx, Config* cfg, const char* workspace_path, int argc, char** argv) {
    (void)cfg;
    (void)workspace_path;
    (void)argc;
    (void)argv;
    if (!ctx || !ctx->stop_active_tasks || !ctx->send_response) return -1;
    
    int cancelled = ctx->stop_active_tasks(ctx);
    char response[256];
    snprintf(response, sizeof(response), cancelled > 0 ? "Stopped %d task(s)." : "No active task to stop.", cancelled);
    ctx->send_response(ctx, response);
    return cancelled;
}

// Restart command handler
static int cmd_restart(CommandContext* ctx, Config* cfg, const char* workspace_path, int argc, char** argv) {
    (void)cfg;
    (void)workspace_path;
    (void)argc;
    (void)argv;
    if (!ctx || !ctx->send_response) return -1;
    
    ctx->send_response(ctx, "Restarting...");
    log_debug("[CommandDispatcher] Restart requested but requires external wrapper");
    return 0;
}

// New session command handler
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

// Tools command handler
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

// Reload plugins command handler
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

// Parse slash command from message content

// Handle plugin commands
static bool handle_plugin_command(AgentLoop* loop, const char* channel, const char* chat_id, const char* session_key, const char* full_content) {
    if (!loop || !channel || !chat_id || !session_key || !full_content) return false;
    
    char cmd_name[64] = {0};
    const char* args_start = NULL;
    parse_slash_command(full_content, cmd_name, sizeof(cmd_name), &args_start);
    
    // Search for matching command
    CommandFunc handler = NULL;
    for (size_t i = 0; i < loop->plugin_mgr->command_count; i++) {
        if (strcmp(loop->plugin_mgr->commands[i].name, cmd_name) == 0) {
            handler = loop->plugin_mgr->commands[i].handler;
            break;
        }
    }
    
    if (handler) {
        log_debug("[CommandDispatcher] Executing command: /%s", cmd_name);
        
        CommandContext* ctx = command_context_new(loop, channel, chat_id, session_key);
        if (!ctx) {
            log_error("[CommandDispatcher] Failed to create command context");
            return false;
        }
        
        int argc = 0;
        char* argv[16] = {0};
        char* arg_copy = NULL;
        if (args_start) {
            arg_copy = strdup(args_start);
            if (arg_copy) {
                char* saveptr = NULL;
                char* token = strtok_r(arg_copy, " ", &saveptr);
                while (token && argc < 15) {
                    argv[argc++] = token;
                    token = strtok_r(NULL, " ", &saveptr);
                }
            }
        }
        
        int result = handler(ctx, loop->config, loop->workspace_path, argc, argv);
        log_debug("[CommandDispatcher] Command /%s executed with result: %d", cmd_name, result);
        
        if (arg_copy) {
            free(arg_copy);
        }
        
        command_context_free(ctx);
        
        return true;
    }
    
    return false;
}

// Handle tool fallback commands
static bool handle_tool_fallback_command(AgentLoop* loop, InboundMessage* inbound, const char* full_content) {
    if (!loop || !inbound || !full_content) return false;
    
    // Use tool router to handle tool fallback
    if (loop->tool_router) {
        return tool_router_handle_fallback_command(loop->tool_router, inbound, full_content);
    }
    
    return false;
}

// Handle skill fallback commands
static bool handle_skill_fallback_command(AgentLoop* loop, InboundMessage* inbound, Session* session, const char* session_key, const char* full_content) {
    if (!loop || !inbound || !session || !session_key || !full_content) return false;
    
    char skill_name[64] = {0};
    const char* args_start = NULL;
    parse_slash_command(full_content, skill_name, sizeof(skill_name), &args_start);

    if (loop->ctx_builder && loop->ctx_builder->skills_loader) {
        char* skill_content = skills_loader_load_skill(loop->ctx_builder->skills_loader, skill_name);
        if (skill_content && skill_content[0] != '\0') {
            Message* user_msg = message_new(ROLE_USER, full_content);
            session_add_message(session, user_msg);

            char key[256];
            snprintf(key, sizeof(key), "%s:%s", inbound->channel.data, inbound->chat_id.data);
            session_orchestrator_set_current_session_key(loop->session_orchestrator, key);
            refresh_tool_routes(loop, inbound->channel.data, inbound->chat_id.data);

            char task_id[32];
            snprintf(task_id, sizeof(task_id), "task_%ld_%d", time(NULL), (int)(random() % 10000));
            add_active_task(loop, task_id, key, pthread_self(), inbound, session->messages.count);
            session_orchestrator_update_task_state(loop->session_orchestrator, key, SESSION_STATE_WAITING_LLM);
            session_orchestrator_increment_task_turn(loop->session_orchestrator, key);
            trigger_llm_async(loop, session, key, inbound->channel.data, inbound->chat_id.data);

            free(skill_content);
            return true;
        }
        free(skill_content);
    }

    return false;
}

// Command dispatcher implementation
CommandDispatcher* command_dispatcher_new(struct AgentLoop* loop) {
    if (!loop) return NULL;
    
    CommandDispatcher* dispatcher = calloc(1, sizeof(CommandDispatcher));
    if (!dispatcher) return NULL;
    
    dispatcher->loop = loop;
    return dispatcher;
}

void command_dispatcher_free(CommandDispatcher* dispatcher) {
    if (dispatcher) {
        free(dispatcher);
    }
}

void command_dispatcher_register_builtin_commands(CommandDispatcher* dispatcher) {
    if (!dispatcher || !dispatcher->loop) return;
    
    // Define builtin commands
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
        plugin_register_command(dispatcher->loop->plugin_mgr, NULL,
            builtin_commands[i].name,
            builtin_commands[i].desc,
            builtin_commands[i].handler);
    }
    
    log_debug("[CommandDispatcher] Registered %zu built-in commands", BUILTIN_COMMAND_COUNT);
}

bool command_dispatcher_handle_message(CommandDispatcher* dispatcher, InboundMessage* inbound, Session* session, const char* session_key, const char* content) {
    if (!dispatcher || !dispatcher->loop || !inbound || !content) return false;
    
    AgentLoop* loop = dispatcher->loop;
    
    const char* trimmed = str_trim_left(content);
    if (trimmed[0] == '/') {
        bool handled = handle_plugin_command(loop, inbound->channel.data, inbound->chat_id.data, session_key, trimmed);
        if (!handled) {
            handled = handle_tool_fallback_command(loop, inbound, trimmed);
            if (!handled) {
                handled = handle_skill_fallback_command(loop, inbound, session, session_key, trimmed);
                if (!handled) {
                    char response[256];
                    snprintf(response, sizeof(response), "Unknown command: %s. Type /help for available commands.", trimmed);
                    OutboundMessage* outbound = outbound_message_new(
                        inbound->channel.data, 
                        inbound->chat_id.data, 
                        response
                    );
                    if (outbound) {
                        message_bus_send_outbound(loop->bus, outbound);
                    }
                }
            }
        }
        return true;
    }
    
    return false;
}

CommandContext* command_context_new(struct AgentLoop* loop, const char* channel, const char* chat_id, const char* session_key) {
    if (!loop || !channel || !chat_id || !session_key) return NULL;
    
    // Allocate user data array to hold necessary information
    void** user_data = malloc(4 * sizeof(void*));
    if (!user_data) return NULL;
    
    user_data[0] = loop;
    user_data[1] = (void*)strdup(channel);
    user_data[2] = (void*)strdup(chat_id);
    user_data[3] = (void*)strdup(session_key);
    
    // Create command context
    CommandContext* ctx = calloc(1, sizeof(CommandContext));
    if (!ctx) {
        free(user_data);
        return NULL;
    }
    
    ctx->user_data = user_data;
    ctx->send_response = command_ctx_send_response;
    ctx->stop_active_tasks = command_ctx_stop_active_tasks;
    ctx->reset_session = command_ctx_reset_session;
    ctx->get_registered_commands = command_ctx_get_registered_commands;
    ctx->get_tool_registry = command_ctx_get_tool_registry;
    ctx->get_plugin_manager = command_ctx_get_plugin_manager;
    
    return ctx;
}

void command_context_free(CommandContext* ctx) {
    if (ctx) {
        if (ctx->user_data) {
            void** user_data = (void**)ctx->user_data;
            free(user_data[1]);
            free(user_data[2]);
            free(user_data[3]);
            free(ctx->user_data);
        }
        free(ctx);
    }
}