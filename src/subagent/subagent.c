#include "../include/subagent.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include "../include/common.h"
#include "../include/message.h"
#include "../tools/tool.h"
#include "../tools/builtin_tools_def.h"
#include "../agent/agent_loop.h"
#include "../bus/message_bus.h"
#include "../tools/tools_impl.h"
#include "../providers/llm_provider.h"
#include <sys/time.h>
#include <pthread.h>

// Generate thread-safe task ID
static void generate_subagent_task_id(char* buf, size_t size) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned int seed = (unsigned int)(ts.tv_nsec ^ (unsigned long)pthread_self());
    unsigned int rand_val = rand_r(&seed) & 0xFFFFFFFF;
    snprintf(buf, size, "%08x", rand_val);
}

// Active subagent task tracking
typedef struct SubagentTaskNode {
    char task_id[32];
    char session_key[SESSION_KEY_MAX];  // "channel:chat_id"
    char* origin_channel;
    char* origin_chat_id;
    AgentLoop* loop;
    pthread_t thread;
    bool cancelling;
    struct SubagentTaskNode* next;
} SubagentTaskNode;

struct SubagentManager {
    SubagentSharedContext* shared;

    pthread_mutex_t mutex;
    SubagentTaskNode* active_tasks;
    int active_count;
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
} SubagentTask;

static void* subagent_loop_thread(void* arg) {
    AgentLoop* loop = (AgentLoop*)arg;
    agent_loop_run(loop);
    return NULL;
}

/**
 * Create shared context for subagents
 */
SubagentSharedContext* subagent_shared_context_create(
    LLMProvider* provider,
    const char* workspace,
    void* bus,
    Config* config
) {
    SubagentSharedContext* shared = calloc(1, sizeof(SubagentSharedContext));
    if (!shared) return NULL;
    
    shared->provider = provider;
    shared->workspace = strdup(workspace);
    shared->bus = (MessageBus*)bus;
    shared->config = config;
    
    // Create shared tool registry
    shared->tool_registry = tool_registry_new();
    if (!shared->tool_registry) {
        free(shared->workspace);
        free(shared);
        return NULL;
    }
    
    // Create shared context builder
    shared->ctx_builder = context_builder_new(workspace);
    if (!shared->ctx_builder) {
        tool_registry_free(shared->tool_registry);
        free(shared->workspace);
        free(shared);
        return NULL;
    }
    
    // Register subagent tools
    ToolContext* tool_ctx = tool_context_new(shared->bus, NULL, NULL, NULL, NULL,
                                             NULL, NULL, shared->workspace);
    if (tool_ctx) {
        snprintf(tool_ctx->current_channel, sizeof(tool_ctx->current_channel), "%s", "subagent");
        /* route_mutex was already initialized by tool_context_new */
        
        for (size_t i = 0; i < BUILTIN_TOOLS_SUBAGENT_COUNT; i++) {
            ToolContext* tool_ctx_copy = malloc(sizeof(ToolContext));
            if (tool_ctx_copy) {
                memcpy(tool_ctx_copy, tool_ctx, sizeof(ToolContext));
                pthread_mutex_init(&tool_ctx_copy->route_mutex, NULL);
                tool_registry_register_full(shared->tool_registry, BUILTIN_TOOLS_SUBAGENT[i].name, BUILTIN_TOOLS_SUBAGENT[i].desc,
                    BUILTIN_TOOLS_SUBAGENT[i].params, BUILTIN_TOOLS_SUBAGENT[i].exec, tool_ctx_copy, NULL, tool_context_destroy);
            }
        }
        pthread_mutex_destroy(&tool_ctx->route_mutex);
        free(tool_ctx);
    }
    
    printf("[Subagent] Shared context created with %zu tools\n", shared->tool_registry->count);
    return shared;
}

/**
 * Destroy shared context
 */
void subagent_shared_context_destroy(SubagentSharedContext* shared) {
    if (!shared) return;
    
    if (shared->tool_registry) {
        tool_registry_free(shared->tool_registry);
    }
    if (shared->ctx_builder) {
        context_builder_free(shared->ctx_builder);
    }
    free(shared->workspace);
    free(shared);
    printf("[Subagent] Shared context destroyed\n");
}

/**
 * Add a task to the active tasks list
 */
static void subagent_add_task(SubagentManager* manager, SubagentTaskNode* node) {
    pthread_mutex_lock(&manager->mutex);
    node->next = manager->active_tasks;
    manager->active_tasks = node;
    manager->active_count++;
    pthread_mutex_unlock(&manager->mutex);
}

/**
 * Remove a task from the active tasks list
 */
static void subagent_remove_task(SubagentManager* manager, const char* task_id) {
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
            manager->active_count--;
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
    SubagentSharedContext* shared = mgr->shared;

    // Create tracking node
    SubagentTaskNode* node = malloc(sizeof(SubagentTaskNode));
    if (node) {
        snprintf(node->task_id, sizeof(node->task_id), "%s", task_data->task_id);
        snprintf(node->session_key, sizeof(node->session_key), "subagent:%s", task_data->task_id);
        node->origin_channel = strdup(task_data->origin_channel);
        node->origin_chat_id = strdup(task_data->origin_chat_id);
        node->loop = NULL;  // Will be set after agent_loop_new
        node->cancelling = false;
        node->next = NULL;
        subagent_add_task(mgr, node);
    }

    printf("[Subagent %s] Initializing...\n", task_data->task_id);

    // 1. Initialize components
    task_data->sub_bus = message_bus_new();

    task_data->session_mgr = session_manager_new(shared->workspace);

    // Create a copy of the tool registry for thread safety
    ToolRegistry* sub_tool_registry = tool_registry_new();
    if (sub_tool_registry && shared->tool_registry) {
        for (size_t i = 0; i < shared->tool_registry->count; i++) {
            Tool* src = &shared->tool_registry->tools[i];
            void* user_data = src->user_data;
            void* plugin_ref = src->plugin_ref;
            ToolUserDataDestroyFunc destroy = NULL;

            if (src->user_data && src->plugin_ref == NULL) {
                ToolContext* base_ctx = (ToolContext*)src->user_data;
                if (base_ctx->magic == TOOL_CONTEXT_MAGIC) {
                    ToolContext* tool_ctx_copy = tool_context_clone_with_route(base_ctx, "subagent", task_data->task_id);
                    if (!tool_ctx_copy) continue;
                    user_data = tool_ctx_copy;
                    plugin_ref = NULL;
                    destroy = tool_context_destroy;
                }
            }
            tool_registry_register_full(sub_tool_registry, src->def.name.data, src->def.description.data,
                src->def.parameters.data, src->execute, user_data, plugin_ref, destroy);
        }
    }

    // Create Loop with own tool registry copy for thread safety
    task_data->loop = agent_loop_new(
        task_data->session_mgr,
        shared->ctx_builder,
        sub_tool_registry ? sub_tool_registry : shared->tool_registry,
        task_data->sub_bus,
        shared->config,
        NULL,
        shared->workspace
    );

    // Update tracking node with loop pointer
    if (node) {
        node->loop = task_data->loop;
    }

    // Set Provider async
    agent_loop_set_llm_provider(task_data->loop, shared->provider);

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
        task_data->task,
        NULL
    );
    message_bus_send_inbound(task_data->sub_bus, task_msg);

    // 4. Relay Outbound Messages (Main Thread of Subagent Task)
    uint64_t start_ms = 0;
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        start_ms = (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
    }
    uint64_t timeout_ms = 300000;  // 5 minutes timeout
    int message_count = 0;
    bool loop_exited = false;

    // Phase 1: Relay messages while agent loop is running
    while (!loop_exited) {
        if (!atomic_load(&task_data->loop->running)) {
            loop_exited = true;
            break;
        }
        OutboundMessage* out = message_bus_receive_outbound_timed(task_data->sub_bus, 1000);
        if (out) {
            char content[2048];
            snprintf(content, sizeof(content), "[Subagent %s]: %s", task_data->label, out->content.data);

            OutboundMessage* relayed = outbound_message_new(
                task_data->origin_channel,
                task_data->origin_chat_id,
                content
            );
            message_bus_send_outbound(shared->bus, relayed);

            outbound_message_free(out);
            message_count++;
        }
        uint64_t now_ms = 0;
        {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            now_ms = (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
        }
        if (now_ms - start_ms > timeout_ms) {
            printf("[Subagent %s] Timed out after %llu ms\n", task_data->task_id,
                   (unsigned long long)(now_ms - start_ms));
            agent_loop_stop(task_data->loop);
            loop_exited = true;
            break;
        }
    }

    // Phase 2: Drain remaining messages from queue (race condition fix)
    // The agent loop may have sent final messages just before setting running=false
    int drain_attempts = 0;
    int max_drain_attempts = 5;
    while (drain_attempts < max_drain_attempts) {
        OutboundMessage* out = message_bus_receive_outbound_timed(task_data->sub_bus, 200);
        if (out) {
            char content[2048];
            snprintf(content, sizeof(content), "[Subagent %s]: %s", task_data->label, out->content.data);

            OutboundMessage* relayed = outbound_message_new(
                task_data->origin_channel,
                task_data->origin_chat_id,
                content
            );
            message_bus_send_outbound(shared->bus, relayed);

            outbound_message_free(out);
            message_count++;
            drain_attempts = 0;
        } else {
            drain_attempts++;
        }
    }

    // 5. Cleanup
    pthread_join(loop_tid, NULL);

    agent_loop_free(task_data->loop);
    session_manager_free(task_data->session_mgr);
    message_bus_free(task_data->sub_bus);

    char saved_task_id[32];
    strncpy(saved_task_id, task_data->task_id, sizeof(saved_task_id) - 1);
    saved_task_id[sizeof(saved_task_id) - 1] = '\0';

    free(task_data->task);
    free(task_data->label);
    free(task_data->origin_channel);
    free(task_data->origin_chat_id);
    free(task_data);

    // Remove from active tasks
    if (node) {
        subagent_remove_task(mgr, saved_task_id);
    }

    printf("[Subagent %s] Finished with %d messages\n", saved_task_id, message_count);
    return NULL;
}

SubagentManager* subagent_manager_create(
    SubagentSharedContext* shared
) {
    SubagentManager* manager = malloc(sizeof(SubagentManager));
    if (!manager) return NULL;

    manager->shared = shared;
    manager->active_tasks = NULL;
    manager->active_count = 0;
    pthread_mutex_init(&manager->mutex, NULL);

    printf("[Subagent] Manager created with shared context\n");
    return manager;
}

void subagent_manager_destroy(SubagentManager* manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->mutex);
    SubagentTaskNode* current = manager->active_tasks;
    while (current) {
        if (current->loop) {
            agent_loop_stop(current->loop);
        }
        current = current->next;
    }
    pthread_mutex_unlock(&manager->mutex);

    int wait_attempts = 0;
    while (wait_attempts < 100) {
        pthread_mutex_lock(&manager->mutex);
        int count = manager->active_count;
        pthread_mutex_unlock(&manager->mutex);
        if (count == 0) break;
        usleep(100000);
        wait_attempts++;
    }

    pthread_mutex_lock(&manager->mutex);
    current = manager->active_tasks;
    while (current) {
        SubagentTaskNode* next = current->next;
        free(current->origin_channel);
        free(current->origin_chat_id);
        free(current);
        current = next;
    }
    manager->active_tasks = NULL;
    manager->active_count = 0;
    pthread_mutex_unlock(&manager->mutex);

    pthread_mutex_destroy(&manager->mutex);
    free(manager);

    printf("[Subagent] Manager destroyed\n");
}

char* subagent_manager_spawn(
    SubagentManager* manager,
    const SubagentSpawnRequest* request
) {
    if (!manager || !request) return NULL;

    pthread_mutex_lock(&manager->mutex);
    if (manager->active_count >= 8) {
        pthread_mutex_unlock(&manager->mutex);
        char* response = malloc(256);
        if (response) snprintf(response, 256, "Cannot spawn subagent: maximum concurrent subagents (8) reached. Wait for existing tasks to complete.");
        return response;
    }
    pthread_mutex_unlock(&manager->mutex);

    // Generate task ID
    char task_id[32];
    generate_subagent_task_id(task_id, sizeof(task_id));

    // Create task data
    SubagentTask* task_data = malloc(sizeof(SubagentTask));
    if (!task_data) return NULL;

    task_data->manager = manager;
    snprintf(task_data->task_id, sizeof(task_data->task_id), "%s", task_id);
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
