#ifndef CHANNEL_H
#define CHANNEL_H

#include "common.h"
#include "message.h"
#include "../bus/message_bus.h"
#include "../include/config.h"

typedef struct Channel Channel;

// Channel interface
struct Channel {
    char* name;
    bool (*init)(Channel* self, Config* cfg, MessageBus* bus);
    void (*start)(Channel* self); // Should run in background or thread
    void (*stop)(Channel* self);
    void (*send)(Channel* self, OutboundMessage* msg);
    void (*destroy)(Channel* self);
    void* user_data;
};

// Factory methods
Channel* channel_create_console();
Channel* channel_create_telegram();
Channel* channel_create_email();
Channel* channel_create_discord();
Channel* channel_create_slack();
Channel* channel_create_dingtalk();
Channel* channel_create_feishu();
// Channel* channel_create_whatsapp(); // Future

#endif // CHANNEL_H
