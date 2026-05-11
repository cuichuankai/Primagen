#ifndef CHANNEL_H
#define CHANNEL_H

#include "common.h"
#include "message.h"
#include "../bus/message_bus.h"
#include "../include/config.h"

// Forward declarations for plugin support
struct PluginManager;
struct LoadedPlugin;

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
    void* plugin_ref;  // Reference to loaded plugin (for cleanup)
    struct PluginManager* plugin_mgr;  // Reference to PluginManager for runtime enable/disable
};

// Channel factory function type
typedef Channel* (*ChannelCreateFunc)(void);

// Channel plugin definition
typedef struct ChannelPluginDef {
    char* name;
    char* description;
    ChannelCreateFunc create;
} ChannelPluginDef;

// Factory methods - Builtin channels (compiled into main binary)
Channel* channel_create_console();

// Note: Feishu and other channels are now external plugins:
//   - feishu: plugins/channels/feishu_channel/feishu_channel.so
//   - telegram: plugins/channels/telegram_channel/telegram_channel.so
//   - email: plugins/channels/email_channel/email_channel.so
//   - discord: plugins/channels/discord_channel/discord_channel.so
//   - slack: plugins/channels/slack_channel/slack_channel.so
//   - dingtalk: plugins/channels/dingtalk_channel/dingtalk_channel.so
// These are loaded dynamically at runtime via plugin_manager_load_external()

// Plugin registration helper
int channel_register_plugin_channel(struct PluginManager* manager, struct LoadedPlugin* plugin,
                                    const char* name, ChannelCreateFunc create);

#endif // CHANNEL_H
