#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include <stdbool.h>

typedef struct HeartbeatService HeartbeatService;

typedef char* (*HeartbeatExecuteCallback)(const char* tasks);
typedef void (*HeartbeatNotifyCallback)(const char* response);

HeartbeatService* heartbeat_service_create(
    const char* workspace,
    void* provider,  // LLMProvider stub
    const char* model,
    HeartbeatExecuteCallback on_execute,
    HeartbeatNotifyCallback on_notify,
    int interval_s,
    bool enabled
);

void heartbeat_service_destroy(HeartbeatService* service);

bool heartbeat_service_start(HeartbeatService* service);
void heartbeat_service_stop(HeartbeatService* service);

#endif // HEARTBEAT_H