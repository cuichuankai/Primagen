/*
 * Stdout Channel Plugin Demo
 *
 * A simple plugin that demonstrates the Primagen channel plugin API.
 * This channel sends messages to stdout with a custom prefix.
 *
 * Build: make
 * Install: cp stdout_channel.so ../../.primagen/plugins/external/
 */

#include "../../../src/include/channel.h"
#include "../../../src/include/common.h"
#include "../../../src/include/logger.h"
#include "../../../src/plugin/plugin_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// =============================================================================
// Channel Data
// =============================================================================

typedef struct {
    MessageBus* bus;
    pthread_t thread_id;
    bool running;
    char* prefix;
} StdoutChannelData;

// =============================================================================
// Channel Implementation
// =============================================================================

static void stdout_send(Channel* self, OutboundMessage* msg) {
    if (!self || !msg) return;

    StdoutChannelData* data = (StdoutChannelData*)self->user_data;

    // Only handle messages for this channel
    if (strcmp(msg->channel.data, "stdout") != 0) {
        return;
    }

    // Print with custom prefix
    printf("\n%s%s%s\n",
           data->prefix ? data->prefix : "[Stdout]",
           msg->content.data,
           data->prefix ? "" : "");
    fflush(stdout);

    log_info("[StdoutChannel] Sent message: %s", msg->content.data);
}

static bool stdout_init(Channel* self, Config* cfg, MessageBus* bus) {
    (void)cfg;

    StdoutChannelData* data = malloc(sizeof(StdoutChannelData));
    if (!data) return false;

    data->bus = bus;
    data->running = false;
    data->prefix = strdup("[Primagen]");
    self->user_data = data;

    log_info("[StdoutChannel] Initialized with prefix: %s", data->prefix);
    return true;
}

static void stdout_start(Channel* self) {
    StdoutChannelData* data = (StdoutChannelData*)self->user_data;
    data->running = true;
    log_info("[StdoutChannel] Started");
}

static void stdout_stop(Channel* self) {
    StdoutChannelData* data = (StdoutChannelData*)self->user_data;
    data->running = false;
    log_info("[StdoutChannel] Stopped");
}

static void stdout_destroy(Channel* self) {
    if (self->user_data) {
        StdoutChannelData* data = (StdoutChannelData*)self->user_data;
        free(data->prefix);
        free(data);
    }
    log_info("[StdoutChannel] Destroyed");
}

// =============================================================================
// Channel Factory
// =============================================================================

static Channel* stdout_channel_create(void) {
    Channel* channel = calloc(1, sizeof(Channel));
    if (!channel) return NULL;

    channel->name = strdup("stdout");
    channel->init = stdout_init;
    channel->start = stdout_start;
    channel->stop = stdout_stop;
    channel->send = stdout_send;
    channel->destroy = stdout_destroy;
    channel->user_data = NULL;
    channel->plugin_ref = NULL;

    log_info("[StdoutChannel] Created new channel instance");
    return channel;
}

// =============================================================================
// Plugin Initialization
// =============================================================================

PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context) {
    (void)context;

    log_info("[Plugin:stdout_channel] Initializing stdout channel plugin");

    // Register the channel factory
    int ret = plugin_register_channel(manager, NULL, "stdout", stdout_channel_create);

    if (ret == 0) {
        log_info("[Plugin:stdout_channel] Successfully registered stdout channel");
    } else {
        log_error("[Plugin:stdout_channel] Failed to register stdout channel");
        return -1;
    }

    return 0;
}

PLUGIN_EXPORT int plugin_cleanup(void) {
    log_info("[Plugin:stdout_channel] Cleaning up stdout channel plugin");
    return 0;
}

// =============================================================================
// Plugin Information
// =============================================================================

static PluginInfo g_plugin_info = {
    .version = 1,
    .type = PLUGIN_CHANNEL,
    .name = "stdout_channel",
    .description = "A demo channel plugin that outputs messages to stdout",
    .plugin_id = "stdout_channel_demo"
};

PLUGIN_EXPORT PluginInfo* plugin_get_info(void) {
    return &g_plugin_info;
}
