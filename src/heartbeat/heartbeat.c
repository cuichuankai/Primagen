#include "../include/heartbeat.h"
#include "../include/logger.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>
#include "../include/common.h"

struct HeartbeatService {
    char* workspace;
    void* provider;
    char* model;
    HeartbeatExecuteCallback on_execute;
    HeartbeatNotifyCallback on_notify;
    int interval_s;
    bool enabled;
    atomic_bool running;
    pthread_t worker_thread;
    pthread_mutex_t mutex;
};

// Read HEARTBEAT.md file
static char* read_heartbeat_file(const char* workspace) {
    if (!workspace) return NULL;

    char path[512];
    snprintf(path, sizeof(path), "%s/HEARTBEAT.md", workspace);

    FILE* f = fopen(path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (length <= 0) {
        fclose(f);
        return NULL;
    }

    char* content = malloc(length + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }

    fread(content, 1, length, f);
    content[length] = '\0';
    fclose(f);

    return content;
}

// Phase 1: Ask LLM to decide skip/run via tool call
// Returns: 0 = skip, 1 = run, -1 = error
static int heartbeat_decide(void* provider, const char* model, const char* content, char** tasks_out) {
    (void)provider;  // Stub - real implementation would use provider
    (void)model;     // Stub - real implementation would use model

    if (!content || !tasks_out) return -1;

    log_debug("[Heartbeat] Asking LLM to decide...");

    // Build prompt
    char prompt[4096];
    snprintf(prompt, sizeof(prompt),
        "Review the following HEARTBEAT.md and decide whether there are active tasks.\n\n"
        "%s",
        content
    );

    // Call LLM with tool (this is a simplified stub - real implementation would call LLM)
    // For now, we simulate the decision based on content
    // A full implementation would use the provider's chat_with_retry with tool support

    // Simple heuristic for simulation:
    // - If content contains "pending", "todo", "task", etc., return "run"
    // - Otherwise return "skip"
    const char* run_keywords[] = {"pending", "todo", "task", "active", "waiting", NULL};
    bool should_run = false;

    const char** kw = run_keywords;
    while (*kw) {
        if (strcasestr(content, *kw)) {
            should_run = true;
            break;
        }
        kw++;
    }

    if (should_run) {
        *tasks_out = strdup("Tasks found in HEARTBEAT.md");
        return 1;
    } else {
        *tasks_out = NULL;
        return 0;
    }
}

static void* heartbeat_worker(void* arg) {
    HeartbeatService* service = (HeartbeatService*)arg;

    while (atomic_load(&service->running)) {
        sleep(service->interval_s);

        if (!atomic_load(&service->running)) break;

        // Read HEARTBEAT.md
        char* content = read_heartbeat_file(service->workspace);
        if (!content) {
            log_debug("[Heartbeat] HEARTBEAT.md missing or empty");
            continue;
        }

        log_debug("[Heartbeat] Checking for tasks...");

        // Phase 1: Decide
        char* tasks = NULL;
        int decision = heartbeat_decide(service->provider, service->model, content, &tasks);
        free(content);

        if (decision <= 0) {
            log_debug("[Heartbeat] OK (nothing to report)");
            continue;
        }

        // Phase 2: Execute
        log_debug("[Heartbeat] Tasks found, executing...");
        if (service->on_execute && tasks) {
            char* response = service->on_execute(tasks);
            if (response && service->on_notify) {
                log_debug("[Heartbeat] Completed, delivering response");
                service->on_notify(response);
            }
            free(response);
        }
        free(tasks);
    }

    return NULL;
}

HeartbeatService* heartbeat_service_create(
    const char* workspace,
    void* provider,
    const char* model,
    HeartbeatExecuteCallback on_execute,
    HeartbeatNotifyCallback on_notify,
    int interval_s,
    bool enabled
) {
    HeartbeatService* service = malloc(sizeof(HeartbeatService));
    if (!service) return NULL;

    service->workspace = strdup(workspace);
    service->provider = provider;
    service->model = model ? strdup(model) : NULL;
    service->on_execute = on_execute;
    service->on_notify = on_notify;
    service->interval_s = interval_s;
    service->enabled = enabled;
    atomic_store(&service->running, false);

    pthread_mutex_init(&service->mutex, NULL);

    return service;
}

void heartbeat_service_destroy(HeartbeatService* service) {
    if (!service) return;

    heartbeat_service_stop(service);
    free(service->workspace);
    free(service->model);
    pthread_mutex_destroy(&service->mutex);
    free(service);
}

bool heartbeat_service_start(HeartbeatService* service) {
    if (!service || !service->enabled || atomic_load(&service->running)) return false;

    atomic_store(&service->running, true);

    if (pthread_create(&service->worker_thread, NULL, heartbeat_worker, service) != 0) {
        atomic_store(&service->running, false);
        log_error("[Heartbeat] Failed to start worker thread");
        return false;
    }

    log_debug("[Heartbeat] Started (every %ds)", service->interval_s);
    return true;
}

void heartbeat_service_stop(HeartbeatService* service) {
    if (!service || !atomic_load(&service->running)) return;

    atomic_store(&service->running, false);
    pthread_join(service->worker_thread, NULL);
    log_debug("[Heartbeat] Stopped");
}

/* Manually trigger a heartbeat */
char* heartbeat_service_trigger_now(HeartbeatService* service) {
    if (!service) return NULL;

    char* content = read_heartbeat_file(service->workspace);
    if (!content) return NULL;

    char* tasks = NULL;
    int decision = heartbeat_decide(service->provider, service->model, content, &tasks);
    free(content);

    if (decision <= 0 || !tasks) {
        return NULL;
    }

    char* result = NULL;
    if (service->on_execute) {
        result = service->on_execute(tasks);
    }
    free(tasks);

    return result;
}
