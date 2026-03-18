#include "../include/subagent.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include "../include/common.h"
#include "../include/message.h"
#include "../tools/tool.h"
#include "../agent/agent_loop.h"
#include "../bus/message_bus.h"
#include "../tools/tools_impl.h"
#include "../providers/llm_provider.h"

// Active subagent task tracking
typedef struct SubagentTaskNode {
    char task_id[32];
    char session_key[256];  // "channel:chat_id"
    char* origin_channel;
    char* origin_chat_id;
    AgentLoop* loop;
    pthread_t thread;
    bool cancelling;
    struct SubagentTaskNode* next;
} SubagentTaskNode;

struct SubagentManager {
    void* provider;  // Function pointer
    char* workspace;
    MessageBus* bus; // Main bus
    Config* config;  // Reference to main config

    pthread_mutex_t mutex;
    SubagentTaskNode* active_tasks;  // Track active subagents
};

typedef struct {
    SubagentManager* manager;
    char task_id[32];
    char* task;
    char* label;
    char* origin_channel;
    char* origin_chat_id;

    // Subagent components
    MessageBus* sub_bus;
    AgentLoop* loop;
    SessionManager* session_mgr;
    ContextBuilder* ctx_builder;
    ToolRegistry* tool_reg;
} SubagentTask;

static void* subagent_loop_thread(void* arg) {
    AgentLoop* loop = (AgentLoop*)arg;
    agent_loop_run(loop);
    return NULL;
}

/**
 * Add a task to the active tasks list
 */
static void add_active_task(SubagentManager* manager, SubagentTaskNode* node) {
    pthread_mutex_lock(&manager->mutex);
    node->next = manager->active_tasks;
    manager->active_tasks = node;
    pthread_mutex_unlock(&manager->mutex);
}

/**
 * Remove a task from the active tasks list
 */
static void remove_active_task(SubagentManager* manager, const char* task_id) {
    pthread_mutex_lock(&manager->mutex);
    SubagentTaskNode* current = manager->active_tasks;
    SubagentTaskNode* prev = NULL;

    while (current) {
        if (strcmp(current->task_id, task_id) == 0) {
            if (prev) {
                prev->next = current->next;
            } else {
                manager->active_tasks = current->next;
            }
            free(current->origin_channel);
            free(current->origin_chat_id);
            free(current);
            break;
        }
        prev = current;
        current = current->next;
    }
    pthread_mutex_unlock(&manager->mutex);
}

/**
 * Find and cancel tasks by session key
 */
int subagent_manager_cancel_by_session(SubagentManager* manager, const char* session_key) {
    if (!manager || !session_key) return 0;

    int cancelled = 0;
    pthread_mutex_lock(&manager->mutex);

    SubagentTaskNode* current = manager->active_tasks;
    while (current) {
        if (strcmp(current->session_key, session_key) == 0 && !current->cancelling) {
            current->cancelling = true;
            if (current->loop) {
                agent_loop_stop(current->loop);
                cancelled++;
            }
        }
        current = current->next;
    }

    pthread_mutex_unlock(&manager->mutex);

    if (cancelled > 0) {
        printf("[Subagent] Cancelled %d task(s) for session: %s\n", cancelled, session_key);
    }

    return cancelled;
}

static void* subagent_task_runner(void* arg) {
    SubagentTask* task_data = (SubagentTask*)arg;
    SubagentManager* mgr = task_data->manager;

    // Create tracking node
    SubagentTaskNode* node = malloc(sizeof(SubagentTaskNode));
    if (node) {
        strcpy(node->task_id, task_data->task_id);
        snprintf(node->session_key, sizeof(node->session_key), "subagent:%s", task_data->task_id);
        node->origin_channel = strdup(task_data->origin_channel);
        node->origin_chat_id = strdup(task_data->origin_chat_id);
        node->loop = NULL;  // Will be set after agent_loop_new
        node->cancelling = false;
        node->next = NULL;
        add_active_task(mgr, node);
    }

    printf("[Subagent %s] Initializing...\n", task_data->task_id);

    // 1. Initialize components
    task_data->sub_bus = message_bus_new();

    // Use unique session key
    char session_path[512];
    snprintf(session_path, sizeof(session_path), "%s/sessions/subagent", mgr->workspace);
    // Ensure dir exists (simplified, session manager handles it usually)
    task_data->session_mgr = session_manager_new(mgr->workspace);

    task_data->ctx_builder = context_builder_new(mgr->workspace);
    task_data->tool_reg = tool_registry_new();

    // Register tools directly (subagents don't use PluginManager)
    // Note: ToolContext fields not explicitly set are NULL
    ToolContext tool_ctx = {
        .bus = task_data->sub_bus,
        .subagent_mgr = mgr,
        .cron_service = NULL,
        .skills_loader = NULL,
        .memory = NULL,
        .workspace = mgr->workspace,
        .current_channel = "subagent",
        .current_chat_id = task_data->task_id
    };

    // Register all standard tools
    tool_registry_register(task_data->tool_reg, "read_file", "Read file content",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        tool_read_file, &tool_ctx);
    tool_registry_register(task_data->tool_reg, "write_file", "Write file content",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}",
        tool_write_file, &tool_ctx);
    tool_registry_register(task_data->tool_reg, "edit_file", "Edit file content",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old_str\":{\"type\":\"string\"},\"new_str\":{\"type\":\"string\"}},\"required\":[\"path\",\"old_str\",\"new_str\"]}",
        tool_edit_file, &tool_ctx);
    tool_registry_register(task_data->tool_reg, "list_dir", "List directory contents",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        tool_list_dir, &tool_ctx);
    tool_registry_register(task_data->tool_reg, "exec", "Execute shell command",
        "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}",
        tool_exec, &tool_ctx);
    tool_registry_register(task_data->tool_reg, "web_search", "Search the web",
        "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"count\":{\"type\":\"integer\"}},\"required\":[\"query\"]}",
        tool_web_search, &tool_ctx);
    tool_registry_register(task_data->tool_reg, "web_fetch", "Fetch URL content",
        "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}",
        tool_web_fetch, &tool_ctx);
    tool_registry_register(task_data->tool_reg, "send_message", "Send message to user",
        "{\"type\":\"object\",\"properties\":{\"content\":{\"type\":\"string\"}},\"required\":[\"content\"]}",
        tool_send_message, &tool_ctx);
    tool_registry_register(task_data->tool_reg, "spawn_subagent", "Spawn subagent",
        "{\"type\":\"object\",\"properties\":{\"task\":{\"type\":\"string\"},\"label\":{\"type\":\"string\"}},\"required\":[\"task\"]}",
        tool_spawn, &tool_ctx);

    // Create Loop
    // Subagent uses the SAME config as main agent for now (same API key, model, etc.)
    // We might want to override model/temp for subagents, but let's keep it simple.
    // Note: Subagents don't need plugin manager access, so pass NULL
    task_data->loop = agent_loop_new(
        task_data->session_mgr,
        task_data->ctx_builder,
        task_data->tool_reg,
        task_data->sub_bus,
        mgr->config,
        NULL,
        ".primagen"  // Default workspace path for subagents
    );

    // Update tracking node with loop pointer
    if (node) {
        node->loop = task_data->loop;
    }

    // Set Provider (cast back to function pointer)
    agent_loop_set_llm_provider(task_data->loop, (LLMProvider)mgr->provider);

    // 2. Start Agent Loop Thread
    pthread_t loop_tid;
    pthread_create(&loop_tid, NULL, subagent_loop_thread, task_data->loop);

    // Store thread id for potential cleanup
    if (node) {
        node->thread = loop_tid;
    }

    // 3. Send Task Message
    InboundMessage* task_msg = inbound_message_new(
        "subagent",
        task_data->task_id,
        task_data->task
    );
    message_bus_send_inbound(task_data->sub_bus, task_msg);

    // 4. Relay Outbound Messages (Main Thread of Subagent Task)
    // We listen to sub_bus outbound and forward to mgr->bus
    while (task_data->loop->running) {
        OutboundMessage* out = message_bus_receive_outbound(task_data->sub_bus);
        if (out) {
            // Forward to main bus
            char content[2048];
            snprintf(content, sizeof(content), "[Subagent %s]: %s", task_data->label, out->content.data);

            OutboundMessage* relayed = outbound_message_new(
                task_data->origin_channel,
                task_data->origin_chat_id,
                content
            );
            message_bus_send_outbound(mgr->bus, relayed);

            outbound_message_free(out);

            // For now, assume any response means we are done?
            // Or maybe keep running?
            // Let's keep running for a bit or until explicit stop.
            // But since we have no interactive way to talk to subagent, we stop after first response.
            // This is a simplification.
            agent_loop_stop(task_data->loop);
        }
    }

    // 5. Cleanup
    pthread_join(loop_tid, NULL);

    agent_loop_free(task_data->loop);
    tool_registry_free(task_data->tool_reg);
    context_builder_free(task_data->ctx_builder);
    session_manager_free(task_data->session_mgr);
    message_bus_free(task_data->sub_bus);

    free(task_data->task);
    free(task_data->label);
    free(task_data->origin_channel);
    free(task_data->origin_chat_id);
    free(task_data);

    // Remove from active tasks
    if (node) {
        remove_active_task(mgr, node->task_id);
    }

    printf("[Subagent] Finished.\n");
    return NULL;
}

SubagentManager* subagent_manager_create(
    void* provider,
    const char* workspace,
    void* bus,
    Config* config
) {
    SubagentManager* manager = malloc(sizeof(SubagentManager));
    if (!manager) return NULL;

    manager->provider = provider;
    manager->workspace = strdup(workspace);
    manager->bus = (MessageBus*)bus;
    manager->config = config;

    pthread_mutex_init(&manager->mutex, NULL);

    return manager;
}

void subagent_manager_destroy(SubagentManager* manager) {
    if (!manager) return;

    free(manager->workspace);
    pthread_mutex_destroy(&manager->mutex);
    free(manager);
}

char* subagent_manager_spawn(
    SubagentManager* manager,
    const SubagentSpawnRequest* request
) {
    if (!manager || !request) return NULL;

    // Generate task ID
    char task_id[32];
    snprintf(task_id, sizeof(task_id), "%08x", rand() % 0xFFFFFFFF);

    // Create task data
    SubagentTask* task_data = malloc(sizeof(SubagentTask));
    if (!task_data) return NULL;

    task_data->manager = manager;
    strcpy(task_data->task_id, task_id);
    task_data->task = strdup(request->task);
    task_data->label = request->label ? strdup(request->label) : strndup(request->task, 30);
    task_data->origin_channel = strdup(request->origin_channel);
    task_data->origin_chat_id = strdup(request->origin_chat_id);

    // Start background thread
    pthread_t thread;
    if (pthread_create(&thread, NULL, subagent_task_runner, task_data) != 0) {
        free(task_data->task);
        free(task_data->label);
        free(task_data->origin_channel);
        free(task_data->origin_chat_id);
        free(task_data);
        return NULL;
    }

    pthread_detach(thread);

    char* response = malloc(256);
    if (response) {
        snprintf(response, 256, "Subagent [%s] started (id: %s).",
                task_data->label, task_id);
    }

    return response;
}
