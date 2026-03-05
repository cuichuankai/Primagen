#include "../include/heartbeat.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include "../include/common.h"

struct HeartbeatService {
    char* workspace;
    void* provider;  // LLMProvider stub
    char* model;
    HeartbeatExecuteCallback on_execute;
    HeartbeatNotifyCallback on_notify;
    int interval_s;
    bool enabled;
    bool running;
    pthread_t worker_thread;
    pthread_mutex_t mutex;
};

static void* heartbeat_worker(void* arg) {
    HeartbeatService* service = (HeartbeatService*)arg;

    while (service->running) {
        sleep(service->interval_s);

        if (service->on_execute) {
            // Simulate heartbeat tasks
            const char* tasks = "Check system status and perform maintenance tasks.";
            char* response = service->on_execute(tasks);

            if (response && service->on_notify) {
                service->on_notify(response);
            }

            free(response);
        }
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
    service->running = false;

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
    if (!service || !service->enabled || service->running) return false;

    service->running = true;

    if (pthread_create(&service->worker_thread, NULL, heartbeat_worker, service) != 0) {
        service->running = false;
        return false;
    }

    return true;
}

void heartbeat_service_stop(HeartbeatService* service) {
    if (!service || !service->running) return;

    service->running = false;
    pthread_join(service->worker_thread, NULL);
}