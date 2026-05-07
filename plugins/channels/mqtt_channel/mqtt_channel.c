/*
 * MQTT Channel Plugin
 *
 * A plugin that provides MQTT channel support for Primagen.
 * Subscribes to MQTT topics and processes incoming messages.
 *
 * Build: make
 * Install: make install
 */

#include "../../../src/include/channel.h"
#include "../../../src/include/config.h"
#include "../../../src/include/message.h"
#include "../../../src/bus/message_bus.h"
#include "../../../src/vendor/cJSON/cJSON.h"
#include "../../../src/vendor/mongoose/mongoose.h"
#include "../../../src/include/logger.h"
#include "../../../src/plugin/plugin_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

// =============================================================================
// Channel Data
// =============================================================================

#define MAX_TOPICS 64
#define MQTT_KEEPALIVE_S 60
#define RECONNECT_DELAY_S 5

typedef struct {
    MessageBus* bus;
    pthread_t thread_id;
    bool running;
    bool connected;
    bool subscribed;
    char* broker_url;
    char* client_id;
    char* username;
    char* password;
    char* default_pub_topic;
    char* topics[MAX_TOPICS];
    int num_topics;
    struct mg_mgr mgr;
    struct mg_connection* mqtt_conn;
} MqttChannelData;

// =============================================================================
// Helper: Free MqttChannelData resources
// =============================================================================

static void mqtt_data_free(MqttChannelData* data) {
    if (!data) return;
    free(data->broker_url);
    free(data->client_id);
    free(data->username);
    free(data->password);
    free(data->default_pub_topic);
    for (int i = 0; i < data->num_topics; i++) {
        free(data->topics[i]);
    }
    free(data);
}

// =============================================================================
// MQTT Event Handler
// =============================================================================

static void mqtt_ev_handler(struct mg_connection* c, int ev, void* ev_data) {
    MqttChannelData* data = (MqttChannelData*)c->fn_data;
    if (!data) return;

    if (ev == MG_EV_MQTT_MSG) {
        struct mg_mqtt_message* mm = (struct mg_mqtt_message*)ev_data;

        log_info("[MQTTChannel] Received message on topic: %.*s", (int)mm->topic.len, mm->topic.buf);

        char* topic_str = malloc(mm->topic.len + 1);
        if (!topic_str) return;
        memcpy(topic_str, mm->topic.buf, mm->topic.len);
        topic_str[mm->topic.len] = '\0';

        char* payload_str = malloc(mm->data.len + 1);
        if (!payload_str) {
            free(topic_str);
            return;
        }
        memcpy(payload_str, mm->data.buf, mm->data.len);
        payload_str[mm->data.len] = '\0';

        InboundMessage* msg = inbound_message_new("mqtt", topic_str, payload_str, NULL);
        if (msg) {
            message_bus_send_inbound(data->bus, msg);
        }

        free(topic_str);
        free(payload_str);
    } else if (ev == MG_EV_MQTT_CMD) {
        struct mg_mqtt_message* mm = (struct mg_mqtt_message*)ev_data;
        if (mm->cmd == MQTT_CMD_CONNACK) {
            data->connected = true;
            if (mm->ack == 0) {
                log_info("[MQTTChannel] Connected to broker (CONNACK received)");
            } else {
                log_error("[MQTTChannel] Broker rejected connection, code: %d", mm->ack);
            }
        } else if (mm->cmd == MQTT_CMD_SUBACK) {
            data->subscribed = true;
            log_info("[MQTTChannel] Subscription confirmed (SUBACK received)");
        }
    } else if (ev == MG_EV_CLOSE) {
        if (data->connected) {
            log_warn("[MQTTChannel] Connection closed");
        }
        data->connected = false;
        data->subscribed = false;
        data->mqtt_conn = NULL;
    } else if (ev == MG_EV_ERROR) {
        log_error("[MQTTChannel] Connection error");
        data->connected = false;
        data->subscribed = false;
        data->mqtt_conn = NULL;
    }
}

// =============================================================================
// MQTT Polling Thread
// =============================================================================

static void mqtt_subscribe_all(MqttChannelData* data) {
    for (int i = 0; i < data->num_topics; i++) {
        if (data->topics[i] && strlen(data->topics[i]) > 0) {
            log_info("[MQTTChannel] Subscribing to topic: %s", data->topics[i]);
            struct mg_mqtt_opts sub_opts = {0};
            sub_opts.topic = mg_str(data->topics[i]);
            sub_opts.qos = 1;
            mg_mqtt_sub(data->mqtt_conn, &sub_opts);
        }
    }
}

static void* mqtt_poller(void* arg) {
    Channel* self = (Channel*)arg;
    MqttChannelData* data = (MqttChannelData*)self->user_data;

    log_info("[MQTTChannel] Polling thread started");

    mg_mgr_init(&data->mgr);

    if (!data->broker_url || strlen(data->broker_url) == 0) {
        log_error("[MQTTChannel] No broker URL configured");
        mg_mgr_free(&data->mgr);
        data->running = false;
        return NULL;
    }

    while (data->running) {
        data->connected = false;
        data->subscribed = false;
        data->mqtt_conn = NULL;

        log_info("[MQTTChannel] Connecting to broker: %s", data->broker_url);

        struct mg_mqtt_opts opts = {0};
        opts.user = mg_str(data->username ? data->username : "");
        opts.pass = mg_str(data->password ? data->password : "");
        opts.client_id = mg_str(data->client_id ? data->client_id : "primagen_mqtt_client");
        opts.keepalive = MQTT_KEEPALIVE_S;
        opts.clean = true;

        data->mqtt_conn = mg_mqtt_connect(&data->mgr, data->broker_url, &opts, mqtt_ev_handler, data);

        if (!data->mqtt_conn) {
            log_error("[MQTTChannel] Failed to initiate connection, retrying in %ds...", RECONNECT_DELAY_S);
            for (int i = 0; i < RECONNECT_DELAY_S * 10 && data->running; i++) {
                mg_mgr_poll(&data->mgr, 100);
            }
            continue;
        }

        for (int wait = 0; wait < 50 && data->running && !data->connected; wait++) {
            mg_mgr_poll(&data->mgr, 100);
        }

        if (!data->connected) {
            log_error("[MQTTChannel] Connection timeout, retrying in %ds...", RECONNECT_DELAY_S);
            for (int i = 0; i < RECONNECT_DELAY_S * 10 && data->running; i++) {
                mg_mgr_poll(&data->mgr, 100);
            }
            continue;
        }

        if (data->num_topics > 0) {
            mqtt_subscribe_all(data);

            for (int wait = 0; wait < 30 && data->running && !data->subscribed; wait++) {
                mg_mgr_poll(&data->mgr, 100);
            }

            if (!data->subscribed) {
                log_warn("[MQTTChannel] Subscription confirmation not received, continuing anyway");
            }
        }

        while (data->running && data->connected) {
            mg_mgr_poll(&data->mgr, 100);
        }

        if (data->running) {
            log_warn("[MQTTChannel] Connection lost, reconnecting in %ds...", RECONNECT_DELAY_S);
            for (int i = 0; i < RECONNECT_DELAY_S * 10 && data->running; i++) {
                mg_mgr_poll(&data->mgr, 100);
            }
        }
    }

    if (data->mqtt_conn) {
        mg_mqtt_disconnect(data->mqtt_conn, NULL);
    }

    mg_mgr_free(&data->mgr);
    data->mqtt_conn = NULL;
    data->connected = false;

    log_info("[MQTTChannel] Polling thread stopped");
    return NULL;
}

// =============================================================================
// Channel Implementation
// =============================================================================

static bool mqtt_init(Channel* self, Config* config, MessageBus* bus) {
    MqttChannelData* data = calloc(1, sizeof(MqttChannelData));
    if (!data) return false;

    data->bus = bus;
    data->running = false;
    data->connected = false;
    data->subscribed = false;
    data->num_topics = 0;
    data->mqtt_conn = NULL;
    self->user_data = data;

    PluginConfig* plugin_cfg = config_get_plugin_config(config, "mqtt_channel");
    if (plugin_cfg && plugin_cfg->config) {
        cJSON* broker_url = cJSON_GetObjectItem(plugin_cfg->config, "broker_url");
        if (broker_url && cJSON_IsString(broker_url) && strlen(broker_url->valuestring) > 0) {
            data->broker_url = strdup(broker_url->valuestring);
        }

        cJSON* client_id = cJSON_GetObjectItem(plugin_cfg->config, "client_id");
        if (client_id && cJSON_IsString(client_id) && strlen(client_id->valuestring) > 0) {
            data->client_id = strdup(client_id->valuestring);
        }

        cJSON* username = cJSON_GetObjectItem(plugin_cfg->config, "username");
        if (username && cJSON_IsString(username) && strlen(username->valuestring) > 0) {
            data->username = strdup(username->valuestring);
        }

        cJSON* password = cJSON_GetObjectItem(plugin_cfg->config, "password");
        if (password && cJSON_IsString(password) && strlen(password->valuestring) > 0) {
            data->password = strdup(password->valuestring);
        }

        cJSON* default_pub_topic = cJSON_GetObjectItem(plugin_cfg->config, "default_pub_topic");
        if (default_pub_topic && cJSON_IsString(default_pub_topic) && strlen(default_pub_topic->valuestring) > 0) {
            data->default_pub_topic = strdup(default_pub_topic->valuestring);
        }

        cJSON* topics = cJSON_GetObjectItem(plugin_cfg->config, "topics");
        if (cJSON_IsArray(topics)) {
            cJSON* topic;
            cJSON_ArrayForEach(topic, topics) {
                if (cJSON_IsString(topic) && data->num_topics < MAX_TOPICS) {
                    data->topics[data->num_topics] = strdup(topic->valuestring);
                    data->num_topics++;
                }
            }
            if (cJSON_GetArraySize(topics) > MAX_TOPICS) {
                log_warn("[MQTTChannel] Too many topics configured (max %d), extras ignored", MAX_TOPICS);
            }
        } else if (cJSON_IsString(topics)) {
            data->topics[data->num_topics] = strdup(topics->valuestring);
            data->num_topics++;
        }
    }

    if (!data->broker_url) {
        log_info("[MQTTChannel] No broker URL configured, skipping initialization");
        mqtt_data_free(data);
        self->user_data = NULL;
        return false;
    }

    log_info("[MQTTChannel] Initialized with broker: %s, topics: %d", data->broker_url, data->num_topics);
    return true;
}

static void mqtt_start(Channel* self) {
    MqttChannelData* data = (MqttChannelData*)self->user_data;
    if (!data) return;
    data->running = true;
    pthread_create(&data->thread_id, NULL, mqtt_poller, self);
    log_info("[MQTTChannel] Started");
}

static void mqtt_stop(Channel* self) {
    MqttChannelData* data = (MqttChannelData*)self->user_data;
    if (data) {
        data->running = false;
        pthread_join(data->thread_id, NULL);
    }
    log_info("[MQTTChannel] Stopped");
}

static void mqtt_send(Channel* self, OutboundMessage* msg) {
    MqttChannelData* data = (MqttChannelData*)self->user_data;
    if (!data || !data->mqtt_conn || !data->connected) return;

    if (strcmp(msg->channel.data, "mqtt") != 0) return;

    if (!msg->content.data || strlen(msg->content.data) == 0) return;

    char* topic = msg->chat_id.data;
    if (!topic || strlen(topic) == 0) {
        topic = data->default_pub_topic ? data->default_pub_topic : "primagen/default";
    }

    struct mg_mqtt_opts opts = {0};
    opts.topic = mg_str(topic);
    opts.message = mg_str(msg->content.data);
    opts.qos = 1;

    uint16_t msg_id = mg_mqtt_pub(data->mqtt_conn, &opts);
    log_info("[MQTTChannel] Published message to %s (id: %u)", topic, msg_id);
}

static void mqtt_destroy(Channel* self) {
    if (self->user_data) {
        MqttChannelData* data = (MqttChannelData*)self->user_data;
        mqtt_data_free(data);
        self->user_data = NULL;
    }
    free(self);
    log_info("[MQTTChannel] Destroyed");
}

// =============================================================================
// Channel Factory
// =============================================================================

static Channel* mqtt_channel_create(void) {
    Channel* channel = calloc(1, sizeof(Channel));
    if (!channel) return NULL;

    channel->name = strdup("mqtt");
    channel->init = mqtt_init;
    channel->start = mqtt_start;
    channel->stop = mqtt_stop;
    channel->send = mqtt_send;
    channel->destroy = mqtt_destroy;
    channel->user_data = NULL;
    channel->plugin_ref = NULL;

    log_info("[MQTTChannel] Created new channel instance");
    return channel;
}

// =============================================================================
// Plugin Initialization
// =============================================================================

PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context) {
    (void)context;

    log_info("[Plugin:mqtt_channel] Initializing mqtt channel plugin");

    int ret = plugin_register_channel(manager, NULL, "mqtt", mqtt_channel_create);

    if (ret == 0) {
        log_info("[Plugin:mqtt_channel] Successfully registered mqtt channel");
    } else {
        log_error("[Plugin:mqtt_channel] Failed to register mqtt channel");
        return -1;
    }

    return 0;
}

PLUGIN_EXPORT int plugin_cleanup(void) {
    log_info("[Plugin:mqtt_channel] Cleaning up mqtt channel plugin");
    return 0;
}

// =============================================================================
// Plugin Information
// =============================================================================

static PluginInfo g_plugin_info = {
    .version = 1,
    .type = PLUGIN_CHANNEL,
    .name = "mqtt_channel",
    .description = "MQTT channel support for Primagen",
    .plugin_id = "mqtt_channel"
};

PLUGIN_EXPORT PluginInfo* plugin_get_info(void) {
    return &g_plugin_info;
}
