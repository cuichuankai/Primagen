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

struct SubagentManager {
    void* provider;  // Function pointer
    char* workspace;
    MessageBus* bus; // Main bus
    Config* config;  // Reference to main config

    pthread_mutex_t mutex;
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

static void* subagent_task_runner(void* arg) {
    SubagentTask* task_data = (SubagentTask*)arg;
    SubagentManager* mgr = task_data->manager;

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
    
    // Register tools
    ToolContext tool_ctx = {
        .bus = task_data->sub_bus, // Subagent uses its own bus for tool output
        .subagent_mgr = mgr,
        .cron_service = NULL // Subagents don't manage cron? Or maybe they do. For now NULL.
    };
    register_all_tools(task_data->tool_reg, &tool_ctx);
    
    // Create Loop
    // Subagent uses the SAME config as main agent for now (same API key, model, etc.)
    // We might want to override model/temp for subagents, but let's keep it simple.
    task_data->loop = agent_loop_new(
        task_data->session_mgr, 
        task_data->ctx_builder, 
        task_data->tool_reg, 
        task_data->sub_bus,
        mgr->config
    );
    
    // Set Provider (cast back to function pointer)
    agent_loop_set_llm_provider(task_data->loop, (LLMProvider)mgr->provider);

    // 2. Start Agent Loop Thread
    pthread_t loop_tid;
    pthread_create(&loop_tid, NULL, subagent_loop_thread, task_data->loop);
    
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

int subagent_manager_cancel_by_session(SubagentManager* manager, const char* session_key) {
    (void)manager;
    (void)session_key;
    return 0;
}
