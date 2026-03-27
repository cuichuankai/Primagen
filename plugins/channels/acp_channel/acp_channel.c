#include "../../../src/include/plugin.h"
#include "../../../src/include/logger.h"
#include "../../../src/include/config.h"
#include "acp.h"
#include "../../../src/vendor/cJSON/cJSON.h"
#include "../../../src/vendor/mongoose/mongoose.h"
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    ACPServer* server;
    pthread_t thread;
    bool running;
    int port;
    char* host;
} ACPChannelData;

static PluginManager* g_plugin_manager = NULL;
static LoadedPlugin* g_plugin_instance = NULL;

static int read_int_cfg(cJSON* root, const char* key, int fallback) {
    if (!root || !key) return fallback;
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(item)) return item->valueint;
    return fallback;
}

static const char* read_str_cfg(cJSON* root, const char* key, const char* fallback) {
    if (!root || !key) return fallback;
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') return item->valuestring;
    return fallback;
}

static void* acp_server_thread(void* arg) {
    ACPChannelData* data = (ACPChannelData*)arg;
    while (data && data->running && data->server && data->server->running) {
        if (data->server->mgr) {
            mg_mgr_poll(data->server->mgr, 100);
        }
    }
    return NULL;
}

static bool acp_init(Channel* self, Config* cfg, MessageBus* bus) {
    if (!self || !cfg || !bus || !g_plugin_manager) return false;
    if (!g_plugin_manager->tool_registry || !g_plugin_manager->agent_loop || !g_plugin_manager->session_mgr) return false;

    ACPChannelData* data = calloc(1, sizeof(ACPChannelData));
    if (!data) return false;

    data->port = ACP_DEFAULT_PORT;
    data->host = strdup(ACP_DEFAULT_HOST);
    if (!data->host) {
        free(data);
        return false;
    }

    PluginConfig* plugin_cfg = config_get_plugin_config(cfg, "acp_channel");
    if (plugin_cfg && plugin_cfg->config) {
        int cfg_port = read_int_cfg(plugin_cfg->config, "port", data->port);
        if (cfg_port > 0 && cfg_port < 65536) data->port = cfg_port;
        const char* cfg_host = read_str_cfg(plugin_cfg->config, "host", data->host);
        if (cfg_host && strcmp(cfg_host, data->host) != 0) {
            free(data->host);
            data->host = strdup(cfg_host);
            if (!data->host) {
                free(data);
                return false;
            }
        }
    }

    data->server = acp_server_new(
        bus,
        g_plugin_manager->tool_registry,
        (AgentLoop*)g_plugin_manager->agent_loop,
        (SessionManager*)g_plugin_manager->session_mgr,
        cfg
    );
    if (!data->server) {
        free(data->host);
        free(data);
        return false;
    }

    self->user_data = data;
    return true;
}

static void acp_start(Channel* self) {
    ACPChannelData* data = self ? (ACPChannelData*)self->user_data : NULL;
    if (!data || data->running || !data->server) return;
    if (acp_server_start(data->server, data->port, data->host) != 0) {
        log_error("[ACPChannel] Failed to start ACP server on %s:%d", data->host, data->port);
        return;
    }
    data->running = true;
    if (pthread_create(&data->thread, NULL, acp_server_thread, data) != 0) {
        data->running = false;
        acp_server_stop(data->server);
        log_error("[ACPChannel] Failed to create ACP poll thread");
        return;
    }
    log_info("[ACPChannel] ACP server started on %s:%d", data->host, data->port);
}

static void acp_stop(Channel* self) {
    ACPChannelData* data = self ? (ACPChannelData*)self->user_data : NULL;
    if (!data || !data->server) return;
    if (data->server->running) acp_server_stop(data->server);
    if (data->running) {
        data->running = false;
        pthread_join(data->thread, NULL);
    }
}

static void acp_send(Channel* self, OutboundMessage* msg) {
    (void)self;
    (void)msg;
}

static void acp_destroy(Channel* self) {
    if (!self || !self->user_data) return;
    ACPChannelData* data = (ACPChannelData*)self->user_data;
    if (data->running || (data->server && data->server->running)) {
        acp_stop(self);
    }
    if (data->server) acp_server_free(data->server);
    free(data->host);
    free(data);
    self->user_data = NULL;
}

static Channel* acp_channel_create(void) {
    Channel* channel = calloc(1, sizeof(Channel));
    if (!channel) return NULL;
    channel->name = strdup("acp");
    channel->init = acp_init;
    channel->start = acp_start;
    channel->stop = acp_stop;
    channel->send = acp_send;
    channel->destroy = acp_destroy;
    channel->plugin_ref = g_plugin_instance;
    return channel;
}

PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context) {
    g_plugin_manager = manager;
    g_plugin_instance = (LoadedPlugin*)context;
    int ret = plugin_register_channel(manager, g_plugin_instance, "acp", acp_channel_create);
    if (ret != 0) {
        log_error("[Plugin:acp_channel] Failed to register ACP channel");
        return -1;
    }
    return 0;
}

PLUGIN_EXPORT int plugin_cleanup(void) {
    g_plugin_manager = NULL;
    g_plugin_instance = NULL;
    return 0;
}

static PluginInfo g_plugin_info = {
    .version = 1,
    .type = PLUGIN_CHANNEL,
    .name = "acp_channel",
    .description = "ACP HTTP server channel plugin",
    .plugin_id = "acp_channel"
};

PLUGIN_EXPORT PluginInfo* plugin_get_info(void) {
    return &g_plugin_info;
}
