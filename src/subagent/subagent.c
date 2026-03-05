#include "../include/subagent.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <uuid/uuid.h>
#include "../include/common.h"
#include "../include/message.h"
#include "../tools/tool.h"

struct SubagentManager {
    void* provider;  // LLMProvider stub
    char* workspace;
    void* bus;       // MessageBus stub
    char* model;
    double temperature;
    int max_tokens;
    char* reasoning_effort;
    char* brave_api_key;
    char* web_proxy;
    bool restrict_to_workspace;

    // Thread management
    pthread_mutex_t mutex;
    // In a real implementation, we'd have task tracking here
};

typedef struct {
    SubagentManager* manager;
    char task_id[9];  // 8 chars + null
    char* task;
    char* label;
    char* origin_channel;
    char* origin_chat_id;
} SubagentTask;

static void* subagent_task_runner(void* arg) {
    SubagentTask* task_data = (SubagentTask*)arg;

    // Simulate subagent execution
    printf("Subagent [%s] starting task: %s\n", task_data->task_id, task_data->label);

    // Simple simulation: just echo the task
    char result[1024];
    snprintf(result, sizeof(result), "Subagent task completed: %s", task_data->task);

    // Announce result (in real implementation, this would send to message bus)
    printf("Subagent [%s] completed: %s\n", task_data->task_id, result);

    // Cleanup
    free(task_data->task);
    free(task_data->label);
    free(task_data->origin_channel);
    free(task_data->origin_chat_id);
    free(task_data);

    return NULL;
}

SubagentManager* subagent_manager_create(
    void* provider,
    const char* workspace,
    void* bus,
    const char* model,
    double temperature,
    int max_tokens,
    const char* reasoning_effort,
    const char* brave_api_key,
    const char* web_proxy,
    bool restrict_to_workspace
) {
    SubagentManager* manager = malloc(sizeof(SubagentManager));
    if (!manager) return NULL;

    manager->provider = provider;
    manager->workspace = strdup(workspace);
    manager->bus = bus;
    manager->model = model ? strdup(model) : NULL;
    manager->temperature = temperature;
    manager->max_tokens = max_tokens;
    manager->reasoning_effort = reasoning_effort ? strdup(reasoning_effort) : NULL;
    manager->brave_api_key = brave_api_key ? strdup(brave_api_key) : NULL;
    manager->web_proxy = web_proxy ? strdup(web_proxy) : NULL;
    manager->restrict_to_workspace = restrict_to_workspace;

    pthread_mutex_init(&manager->mutex, NULL);

    return manager;
}

void subagent_manager_destroy(SubagentManager* manager) {
    if (!manager) return;

    free(manager->workspace);
    free(manager->model);
    free(manager->reasoning_effort);
    free(manager->brave_api_key);
    free(manager->web_proxy);

    pthread_mutex_destroy(&manager->mutex);
    free(manager);
}

char* subagent_manager_spawn(
    SubagentManager* manager,
    const SubagentSpawnRequest* request
) {
    if (!manager || !request) return NULL;

    // Generate task ID (simplified UUID)
    char task_id[9];
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

    // Detach thread so it cleans up automatically
    pthread_detach(thread);

    // Return success message
    char* response = malloc(256);
    if (response) {
        snprintf(response, 256, "Subagent [%s] started (id: %s). I'll notify you when it completes.",
                task_data->label, task_id);
    }

    return response;
}

int subagent_manager_cancel_by_session(SubagentManager* manager, const char* session_key) {
    // Stub implementation - in real version would cancel tasks for session
    (void)manager;
    (void)session_key;
    return 0;
}