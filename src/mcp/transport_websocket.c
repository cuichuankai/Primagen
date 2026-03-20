#include "mcp.h"
#include "transport_internal.h"
#include "../include/logger.h"
#include "../vendor/mongoose/mongoose.h"
#include "../vendor/cJSON/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef struct {
    struct mg_mgr mgr;
    struct mg_connection* conn;
    pthread_t poll_thread;
    bool running;
    bool ws_open;
    bool poll_thread_started;
    bool connect_failed;
    char* error_message;
    char* read_buffer;
    size_t buffer_size;
    size_t buffer_len;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} MCPWebSocketTransport;

#define WS_BUFFER_SIZE 65536
#define WS_CONNECT_TIMEOUT_MS 10000

static void ws_buffer_append(MCPWebSocketTransport* transport, const char* msg) {
    if (!transport || !msg) return;
    size_t msg_len = strlen(msg);
    if (msg_len == 0) return;

    size_t needed = transport->buffer_len + msg_len + 2;
    if (needed >= transport->buffer_size) {
        size_t new_size = needed * 2;
        char* new_buffer = realloc(transport->read_buffer, new_size);
        if (!new_buffer) return;
        transport->read_buffer = new_buffer;
        transport->buffer_size = new_size;
    }

    if (transport->buffer_len > 0) {
        transport->read_buffer[transport->buffer_len++] = '\n';
    }
    memcpy(transport->read_buffer + transport->buffer_len, msg, msg_len);
    transport->buffer_len += msg_len;
    transport->read_buffer[transport->buffer_len] = '\0';
}

static char* ws_buffer_pop(MCPWebSocketTransport* transport) {
    if (!transport || transport->buffer_len == 0) return NULL;
    char* newline = strchr(transport->read_buffer, '\n');
    size_t msg_len = newline ? (size_t)(newline - transport->read_buffer) : transport->buffer_len;
    char* result = malloc(msg_len + 1);
    if (!result) return NULL;

    memcpy(result, transport->read_buffer, msg_len);
    result[msg_len] = '\0';

    size_t consumed = newline ? msg_len + 1 : msg_len;
    size_t remaining = transport->buffer_len - consumed;
    if (remaining > 0) {
        memmove(transport->read_buffer, transport->read_buffer + consumed, remaining);
    }
    transport->buffer_len = remaining;
    transport->read_buffer[transport->buffer_len] = '\0';
    return result;
}

static void mcp_websocket_event_handler(struct mg_connection* c, int ev, void* ev_data) {
    MCPClient* client = (MCPClient*) c->fn_data;
    if (!client || !client->transport_data) return;
    MCPWebSocketTransport* transport = (MCPWebSocketTransport*) client->transport_data;

    if (ev == MG_EV_WS_OPEN) {
        pthread_mutex_lock(&transport->mutex);
        transport->ws_open = true;
        pthread_cond_signal(&transport->cond);
        pthread_mutex_unlock(&transport->mutex);
        return;
    }

    if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message* wm = (struct mg_ws_message*) ev_data;
        if (!wm || wm->data.len == 0) return;

        char* msg = malloc(wm->data.len + 1);
        if (!msg) return;
        memcpy(msg, wm->data.buf, wm->data.len);
        msg[wm->data.len] = '\0';

        cJSON* parsed = cJSON_Parse(msg);
        if (parsed) {
            char* compact = cJSON_PrintUnformatted(parsed);
            pthread_mutex_lock(&transport->mutex);
            if (compact) {
                ws_buffer_append(transport, compact);
                free(compact);
            } else {
                ws_buffer_append(transport, msg);
            }
            pthread_cond_signal(&transport->cond);
            pthread_mutex_unlock(&transport->mutex);
            cJSON_Delete(parsed);
        }
        free(msg);
        return;
    }

    if (ev == MG_EV_ERROR) {
        const char* err = ev_data ? (const char*) ev_data : "WebSocket connection error";
        pthread_mutex_lock(&transport->mutex);
        transport->connect_failed = true;
        free(transport->error_message);
        transport->error_message = strdup(err);
        pthread_cond_signal(&transport->cond);
        pthread_mutex_unlock(&transport->mutex);
        return;
    }

    if (ev == MG_EV_CLOSE) {
        pthread_mutex_lock(&transport->mutex);
        transport->running = false;
        pthread_cond_signal(&transport->cond);
        pthread_mutex_unlock(&transport->mutex);
        return;
    }
}

static void* websocket_poll_thread(void* arg) {
    MCPClient* client = (MCPClient*)arg;
    if (!client || !client->transport_data) return NULL;
    MCPWebSocketTransport* transport = (MCPWebSocketTransport*)client->transport_data;
    while (transport->running) {
        mg_mgr_poll(&transport->mgr, 50);
    }
    return NULL;
}

static Error mcp_transport_websocket_init(MCPClient* client) {
    if (!client || !client->command || strlen(client->command) == 0) {
        return error_new(ERR_INVALID_PARAM, "Invalid websocket URL");
    }

    MCPWebSocketTransport* transport = calloc(1, sizeof(MCPWebSocketTransport));
    if (!transport) {
        return error_new(ERR_MEMORY, "Failed to allocate websocket transport");
    }

    transport->buffer_size = WS_BUFFER_SIZE;
    transport->read_buffer = malloc(transport->buffer_size);
    if (!transport->read_buffer) {
        free(transport);
        return error_new(ERR_MEMORY, "Failed to allocate websocket buffer");
    }
    transport->read_buffer[0] = '\0';
    pthread_mutex_init(&transport->mutex, NULL);
    pthread_cond_init(&transport->cond, NULL);
    mg_mgr_init(&transport->mgr);
    transport->running = true;
    client->transport_data = transport;

    transport->conn = mg_ws_connect(&transport->mgr, client->command, mcp_websocket_event_handler, client, NULL);
    if (!transport->conn) {
        mg_mgr_free(&transport->mgr);
        pthread_mutex_destroy(&transport->mutex);
        pthread_cond_destroy(&transport->cond);
        free(transport->read_buffer);
        free(transport);
        client->transport_data = NULL;
        return error_new(ERR_CONNECTION, "Failed to create websocket connection");
    }

    if (mg_url_is_ssl(client->command)) {
        struct mg_str host = mg_url_host(client->command);
        struct mg_tls_opts opts = {0};
        opts.ca = mg_str("");
        opts.name = host;
        mg_tls_init(transport->conn, &opts);
    }

    if (pthread_create(&transport->poll_thread, NULL, websocket_poll_thread, client) != 0) {
        transport->running = false;
        mg_mgr_free(&transport->mgr);
        pthread_mutex_destroy(&transport->mutex);
        pthread_cond_destroy(&transport->cond);
        free(transport->read_buffer);
        free(transport);
        client->transport_data = NULL;
        return error_new(ERR_CONNECTION, "Failed to start websocket poll thread");
    }
    transport->poll_thread_started = true;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += WS_CONNECT_TIMEOUT_MS / 1000;
    ts.tv_nsec += (WS_CONNECT_TIMEOUT_MS % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    pthread_mutex_lock(&transport->mutex);
    while (!transport->ws_open && !transport->connect_failed && transport->running) {
        int ret = pthread_cond_timedwait(&transport->cond, &transport->mutex, &ts);
        if (ret == ETIMEDOUT) break;
    }
    bool ready = transport->ws_open && !transport->connect_failed;
    char* err_copy = transport->error_message ? strdup(transport->error_message) : NULL;
    pthread_mutex_unlock(&transport->mutex);

    if (!ready) {
        mcp_client_disconnect(client);
        if (err_copy) {
            Error err = error_new(ERR_CONNECTION, err_copy);
            free(err_copy);
            return err;
        }
        return error_new(ERR_TIMEOUT, "WebSocket handshake timeout");
    }

    free(err_copy);
    log_debug("[MCP websocket] Connected to %s", client->server_id);
    return error_new(ERR_NONE, "");
}

static Error mcp_transport_websocket_send(MCPClient* client, const char* data) {
    if (!client || !client->transport_data || !data) {
        return error_new(ERR_INVALID_PARAM, "Invalid client, transport or data");
    }

    MCPWebSocketTransport* transport = (MCPWebSocketTransport*)client->transport_data;
    pthread_mutex_lock(&transport->mutex);
    bool can_send = transport->running && transport->ws_open && transport->conn != NULL;
    pthread_mutex_unlock(&transport->mutex);

    if (!can_send) {
        return error_new(ERR_CONNECTION, "WebSocket transport not connected");
    }

    size_t sent = mg_ws_send(transport->conn, data, strlen(data), WEBSOCKET_OP_TEXT);
    if (sent == 0) {
        return error_new(ERR_CONNECTION, "Failed to send websocket frame");
    }
    return error_new(ERR_NONE, "");
}

static char* mcp_transport_websocket_recv(MCPClient* client, int timeout_ms) {
    if (!client || !client->transport_data) {
        return NULL;
    }

    MCPWebSocketTransport* transport = (MCPWebSocketTransport*)client->transport_data;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    pthread_mutex_lock(&transport->mutex);
    while (transport->buffer_len == 0 && transport->running) {
        int ret = pthread_cond_timedwait(&transport->cond, &transport->mutex, &ts);
        if (ret == ETIMEDOUT) break;
    }
    char* result = ws_buffer_pop(transport);
    pthread_mutex_unlock(&transport->mutex);
    return result;
}

static void mcp_transport_websocket_close(MCPClient* client) {
    if (!client || !client->transport_data) {
        return;
    }

    MCPWebSocketTransport* transport = (MCPWebSocketTransport*)client->transport_data;
    pthread_mutex_lock(&transport->mutex);
    transport->running = false;
    if (transport->conn) {
        mg_ws_send(transport->conn, "", 0, WEBSOCKET_OP_CLOSE);
    }
    pthread_cond_signal(&transport->cond);
    pthread_mutex_unlock(&transport->mutex);

    if (transport->poll_thread_started) {
        pthread_join(transport->poll_thread, NULL);
    }

    mg_mgr_free(&transport->mgr);
    pthread_mutex_destroy(&transport->mutex);
    pthread_cond_destroy(&transport->cond);
    free(transport->error_message);
    free(transport->read_buffer);
    free(transport);
    client->transport_data = NULL;
}

MCPTransportOps* mcp_transport_websocket_ops(void) {
    static MCPTransportOps ops = {
        .init = mcp_transport_websocket_init,
        .send = mcp_transport_websocket_send,
        .recv = mcp_transport_websocket_recv,
        .close = mcp_transport_websocket_close
    };
    return &ops;
}
