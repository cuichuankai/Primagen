#include "mcp.h"
#include "transport_internal.h"
#include "../../../src/include/logger.h"
#include "../../../src/vendor/mongoose/mongoose.h"
#include "../../../src/vendor/cJSON/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>

typedef struct {
    struct mg_mgr mgr;
    struct mg_connection* sse_conn;
    pthread_t poll_thread;
    bool running;
    bool sse_open;
    bool poll_thread_started;
    bool connect_failed;
    char* error_message;
    char* read_buffer;
    size_t buffer_size;
    size_t buffer_len;
    char* recv_buffer;
    size_t recv_buffer_size;
    size_t recv_buffer_len;
    char* event_buffer;
    size_t event_buffer_size;
    size_t event_buffer_len;
    char current_event[64];
    char* post_url;
    char* session_id;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} MCPSSETransport;

typedef struct {
    bool done;
    bool ok;
    int http_status;
    char last_error[256];
    char* response_body;
    char* session_id;
} MCPSSEPostContext;

typedef struct {
    bool done;
    bool ok;
    int http_status;
    char last_error[256];
    char* body;
    size_t body_len;
    bool ssl;
    bool request_sent;
    const char* url;
} MCPSSEDiscoverContext;

#define SSE_BUFFER_SIZE 65536
#define SSE_CONNECT_TIMEOUT_MS 10000
#define SSE_POST_TIMEOUT_MS 30000

static bool is_reserved_http_header(const char* key) {
    if (!key) return false;
    return strcasecmp(key, "Host") == 0 || strcasecmp(key, "Content-Length") == 0;
}

static void append_custom_headers(struct mg_connection* c, MCPClient* client) {
    if (!c || !client) return;
    for (size_t i = 0; i < client->headers.count; i++) {
        EnvVar* h = &client->headers.items[i];
        if (!h->key || !h->value) continue;
        if (is_reserved_http_header(h->key)) continue;
        mg_printf(c, "%s: %s\r\n", h->key, h->value);
    }
}

static char* sse_resolve_url(const char* base_url, const char* endpoint) {
    if (!endpoint || endpoint[0] == '\0') return NULL;
    if (strncmp(endpoint, "http://", 7) == 0 || strncmp(endpoint, "https://", 8) == 0) {
        return strdup(endpoint);
    }
    if (!base_url) return NULL;

    const char* scheme = strstr(base_url, "://");
    if (!scheme) return NULL;
    const char* host_start = scheme + 3;
    const char* path_start = strchr(host_start, '/');
    size_t origin_len = path_start ? (size_t)(path_start - base_url) : strlen(base_url);

    if (endpoint[0] == '/') {
        size_t total = origin_len + strlen(endpoint) + 1;
        char* out = malloc(total);
        if (!out) return NULL;
        memcpy(out, base_url, origin_len);
        out[origin_len] = '\0';
        strcat(out, endpoint);
        return out;
    }

    const char* last_slash = strrchr(base_url, '/');
    size_t base_len = last_slash ? (size_t)(last_slash - base_url + 1) : strlen(base_url);
    size_t total = base_len + strlen(endpoint) + 1;
    char* out = malloc(total);
    if (!out) return NULL;
    memcpy(out, base_url, base_len);
    out[base_len] = '\0';
    strcat(out, endpoint);
    return out;
}

static char* sse_extract_endpoint(const char* body) {
    if (!body) return NULL;
    const char* p = body;
    while ((p = strstr(p, "data:")) != NULL) {
        p += 5;
        while (*p == ' ') p++;
        const char* end = strpbrk(p, "\r\n");
        if (!end) end = p + strlen(p);
        size_t len = (size_t)(end - p);
        if (len > 0) {
            char* out = malloc(len + 1);
            if (!out) return NULL;
            memcpy(out, p, len);
            out[len] = '\0';
            return out;
        }
    }
    return NULL;
}

static void mcp_sse_discover_send_request(struct mg_connection* c, MCPSSEDiscoverContext* ctx) {
    if (!c || !ctx || ctx->request_sent) return;
    struct mg_str host = mg_url_host(ctx->url);
    mg_printf(c,
              "GET %s HTTP/1.1\r\n"
              "Host: %.*s\r\n"
              "Accept: text/event-stream\r\n"
              "Cache-Control: no-cache\r\n"
              "Connection: close\r\n\r\n",
              mg_url_uri(ctx->url), (int) host.len, host.buf);
    ctx->request_sent = true;
}

static void mcp_sse_discover_event_handler(struct mg_connection* c, int ev, void* ev_data) {
    MCPSSEDiscoverContext* ctx = (MCPSSEDiscoverContext*) c->fn_data;
    if (!ctx) return;
    if (ev == MG_EV_CONNECT) {
        int status = *(int*) ev_data;
        if (status != 0) {
            snprintf(ctx->last_error, sizeof(ctx->last_error), "connect failed: %d", status);
            ctx->done = true;
            ctx->ok = false;
            return;
        }
        if (ctx->ssl) {
            struct mg_str host = mg_url_host(ctx->url);
            struct mg_tls_opts opts = {0};
            opts.ca = mg_str("");
            opts.name = host;
            mg_tls_init(c, &opts);
        } else {
            mcp_sse_discover_send_request(c, ctx);
        }
    } else if (ev == MG_EV_TLS_HS) {
        mcp_sse_discover_send_request(c, ctx);
    } else if (ev == MG_EV_ERROR) {
        const char* err = ev_data ? (const char*) ev_data : "SSE discover error";
        snprintf(ctx->last_error, sizeof(ctx->last_error), "%s", err);
        ctx->done = true;
        ctx->ok = false;
    } else if (ev == MG_EV_POLL) {
        if (ctx->done || c->recv.len == 0) return;
        size_t new_len = ctx->body_len + c->recv.len;
        char* new_body = realloc(ctx->body, new_len + 1);
        if (!new_body) return;
        ctx->body = new_body;
        memcpy(ctx->body + ctx->body_len, c->recv.buf, c->recv.len);
        ctx->body_len = new_len;
        ctx->body[ctx->body_len] = '\0';
        mg_iobuf_del(&c->recv, 0, c->recv.len);

        char* body = strstr(ctx->body, "\r\n\r\n");
        if (!body) body = strstr(ctx->body, "\n\n");
        if (body) {
            body = (body[1] == '\n') ? body + 2 : body + 4;
        } else {
            body = ctx->body;
        }

        char* endpoint = sse_extract_endpoint(body);
        if (endpoint) {
            free(ctx->body);
            ctx->body = endpoint;
            ctx->body_len = strlen(endpoint);
            ctx->ok = true;
            ctx->done = true;
            c->is_closing = 1;
        }
    }
}

static void mcp_sse_try_discover_post_url(MCPClient* client, MCPSSETransport* transport) {
    (void) client;
    (void) transport;
}

static void sse_queue_append(MCPSSETransport* transport, const char* msg) {
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

static char* sse_queue_pop(MCPSSETransport* transport) {
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

static bool sse_append_bytes(char** buffer, size_t* buffer_size, size_t* buffer_len, const char* data, size_t data_len) {
    if (!buffer || !buffer_size || !buffer_len || !data) return false;
    size_t needed = *buffer_len + data_len + 1;
    if (needed > *buffer_size) {
        size_t new_size = needed * 2;
        char* new_buffer = realloc(*buffer, new_size);
        if (!new_buffer) return false;
        *buffer = new_buffer;
        *buffer_size = new_size;
    }
    memcpy(*buffer + *buffer_len, data, data_len);
    *buffer_len += data_len;
    (*buffer)[*buffer_len] = '\0';
    return true;
}

static void sse_reset_event_buffer(MCPSSETransport* transport) {
    if (!transport || !transport->event_buffer) return;
    transport->event_buffer_len = 0;
    transport->event_buffer[0] = '\0';
}

static void sse_emit_event_if_ready(MCPClient* client, MCPSSETransport* transport) {
    if (!transport || transport->event_buffer_len == 0) return;
    while (transport->event_buffer_len > 0 &&
           (transport->event_buffer[transport->event_buffer_len - 1] == '\n' ||
            transport->event_buffer[transport->event_buffer_len - 1] == '\r')) {
        transport->event_buffer_len--;
    }
    transport->event_buffer[transport->event_buffer_len] = '\0';
    if (transport->event_buffer_len == 0) return;
    if (strcmp(transport->event_buffer, "[DONE]") == 0) {
        sse_reset_event_buffer(transport);
        transport->current_event[0] = '\0';
        return;
    }

    if (strcmp(transport->current_event, "endpoint") == 0) {
        char* resolved = sse_resolve_url(client ? client->command : NULL, transport->event_buffer);
        if (resolved) {
            pthread_mutex_lock(&transport->mutex);
            free(transport->post_url);
            transport->post_url = resolved;
            log_debug("[MCP sse] Discovered POST endpoint: %s", transport->post_url);
            pthread_cond_signal(&transport->cond);
            pthread_mutex_unlock(&transport->mutex);
        }
        sse_reset_event_buffer(transport);
        transport->current_event[0] = '\0';
        return;
    }

    cJSON* parsed = cJSON_Parse(transport->event_buffer);
    if (parsed) {
        char* compact = cJSON_PrintUnformatted(parsed);
        pthread_mutex_lock(&transport->mutex);
        if (compact) {
            sse_queue_append(transport, compact);
            free(compact);
        } else {
            sse_queue_append(transport, transport->event_buffer);
        }
        pthread_cond_signal(&transport->cond);
        pthread_mutex_unlock(&transport->mutex);
        cJSON_Delete(parsed);
    }
    sse_reset_event_buffer(transport);
    transport->current_event[0] = '\0';
}

static void sse_process_received_data(MCPClient* client, MCPSSETransport* transport, const char* data, size_t data_len) {
    if (!transport || !data || data_len == 0) return;
    if (!sse_append_bytes(&transport->recv_buffer, &transport->recv_buffer_size, &transport->recv_buffer_len, data, data_len)) {
        return;
    }

    char* line_start = transport->recv_buffer;
    char* newline = NULL;
    while ((newline = strchr(line_start, '\n')) != NULL) {
        *newline = '\0';
        size_t line_len = strlen(line_start);
        if (line_len > 0 && line_start[line_len - 1] == '\r') {
            line_start[line_len - 1] = '\0';
            line_len--;
        }

        if (line_len == 0) {
            sse_emit_event_if_ready(client, transport);
        } else if (strncmp(line_start, "event:", 6) == 0) {
            const char* event_name = line_start + 6;
            if (*event_name == ' ') event_name++;
            size_t n = strlen(event_name);
            if (n >= sizeof(transport->current_event)) n = sizeof(transport->current_event) - 1;
            memcpy(transport->current_event, event_name, n);
            transport->current_event[n] = '\0';
        } else if (strncmp(line_start, "data:", 5) == 0) {
            const char* payload = line_start + 5;
            if (*payload == ' ') payload++;
            size_t payload_len = strlen(payload);
            sse_append_bytes(&transport->event_buffer, &transport->event_buffer_size, &transport->event_buffer_len, payload, payload_len);
            sse_append_bytes(&transport->event_buffer, &transport->event_buffer_size, &transport->event_buffer_len, "\n", 1);
        }

        line_start = newline + 1;
    }

    size_t consumed = (size_t)(line_start - transport->recv_buffer);
    if (consumed > 0) {
        size_t remaining = transport->recv_buffer_len - consumed;
        if (remaining > 0) {
            memmove(transport->recv_buffer, transport->recv_buffer + consumed, remaining);
        }
        transport->recv_buffer_len = remaining;
        transport->recv_buffer[transport->recv_buffer_len] = '\0';
    }
}

static void mcp_sse_stream_event_handler(struct mg_connection* c, int ev, void* ev_data) {
    MCPClient* client = (MCPClient*) c->fn_data;
    if (!client || !client->transport_data) return;
    MCPSSETransport* transport = (MCPSSETransport*) client->transport_data;

    if (ev == MG_EV_HTTP_HDRS) {
        struct mg_http_message* hm = (struct mg_http_message*) ev_data;
        int status = mg_http_status(hm);
        pthread_mutex_lock(&transport->mutex);
        if (status >= 200 && status < 300) {
            transport->sse_open = true;
        } else {
            transport->connect_failed = true;
            free(transport->error_message);
            char err[128];
            snprintf(err, sizeof(err), "SSE HTTP status %d", status);
            transport->error_message = strdup(err);
        }
        pthread_cond_signal(&transport->cond);
        pthread_mutex_unlock(&transport->mutex);
        return;
    }

    if (ev == MG_EV_POLL) {
        if (c->recv.len > 0) {
            size_t n = c->recv.len;
            char* buf = malloc(n + 1);
            if (buf) {
                memcpy(buf, c->recv.buf, n);
                buf[n] = '\0';
                sse_process_received_data(client, transport, buf, n);
                free(buf);
            }
            mg_iobuf_del(&c->recv, 0, n);
        }
        return;
    }

    if (ev == MG_EV_ERROR) {
        const char* err = ev_data ? (const char*) ev_data : "SSE connection error";
        pthread_mutex_lock(&transport->mutex);
        transport->connect_failed = true;
        transport->running = false;
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

static void mcp_sse_post_event_handler(struct mg_connection* c, int ev, void* ev_data) {
    MCPSSEPostContext* ctx = (MCPSSEPostContext*) c->fn_data;
    if (!ctx) return;
    if (ev == MG_EV_HTTP_HDRS) {
        struct mg_http_message* hm = (struct mg_http_message*) ev_data;
        ctx->http_status = mg_http_status(hm);
        const char* sid = get_header_value_ci(hm, "mcp-session-id", (char[1024]){}, 1024);
        if (sid && sid[0] != '\0') {
            size_t sid_len = strlen(sid);
            ctx->session_id = malloc(sid_len + 1);
            if (ctx->session_id) {
                memcpy(ctx->session_id, sid, sid_len);
                ctx->session_id[sid_len] = '\0';
            }
        }
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message* hm = (struct mg_http_message*) ev_data;
        int status = mg_http_status(hm);
        ctx->http_status = status;
        ctx->ok = (status >= 200 && status < 300);
        if (hm->body.len > 0) {
            ctx->response_body = malloc(hm->body.len + 1);
            if (ctx->response_body) {
                memcpy(ctx->response_body, hm->body.buf, hm->body.len);
                ctx->response_body[hm->body.len] = '\0';
            }
        }
        ctx->done = true;
        c->is_closing = 1;
    } else if (ev == MG_EV_ERROR) {
        const char* err = ev_data ? (const char*) ev_data : "SSE POST error";
        snprintf(ctx->last_error, sizeof(ctx->last_error), "%s", err);
        ctx->ok = false;
        ctx->done = true;
    } else if (ev == MG_EV_CLOSE) {
        if (!ctx->done) {
            ctx->done = true;
            ctx->ok = (ctx->http_status >= 200 && ctx->http_status < 300);
        }
    }
}

static void* sse_poll_thread(void* arg) {
    MCPClient* client = (MCPClient*) arg;
    if (!client || !client->transport_data) return NULL;
    MCPSSETransport* transport = (MCPSSETransport*) client->transport_data;
    while (transport->running) {
        mg_mgr_poll(&transport->mgr, 50);
    }
    return NULL;
}

static Error mcp_transport_sse_init(MCPClient* client) {
    if (!client || !client->command || strlen(client->command) == 0) {
        return error_new(ERR_INVALID_PARAM, "Invalid SSE URL");
    }

    MCPSSETransport* transport = calloc(1, sizeof(MCPSSETransport));
    if (!transport) {
        return error_new(ERR_MEMORY, "Failed to allocate SSE transport");
    }

    transport->buffer_size = SSE_BUFFER_SIZE;
    transport->read_buffer = malloc(transport->buffer_size);
    transport->recv_buffer_size = SSE_BUFFER_SIZE;
    transport->recv_buffer = malloc(transport->recv_buffer_size);
    transport->event_buffer_size = SSE_BUFFER_SIZE;
    transport->event_buffer = malloc(transport->event_buffer_size);
    if (!transport->read_buffer || !transport->recv_buffer || !transport->event_buffer) {
        free(transport->read_buffer);
        free(transport->recv_buffer);
        free(transport->event_buffer);
        free(transport);
        return error_new(ERR_MEMORY, "Failed to allocate SSE buffers");
    }
    transport->read_buffer[0] = '\0';
    transport->recv_buffer[0] = '\0';
    transport->event_buffer[0] = '\0';
    pthread_mutex_init(&transport->mutex, NULL);
    pthread_cond_init(&transport->cond, NULL);
    mg_mgr_init(&transport->mgr);
    transport->running = true;
    client->transport_data = transport;

    transport->sse_conn = mg_http_connect(&transport->mgr, client->command, mcp_sse_stream_event_handler, client);
    if (!transport->sse_conn) {
        mg_mgr_free(&transport->mgr);
        pthread_mutex_destroy(&transport->mutex);
        pthread_cond_destroy(&transport->cond);
        free(transport->read_buffer);
        free(transport->recv_buffer);
        free(transport->event_buffer);
        free(transport);
        client->transport_data = NULL;
        return error_new(ERR_CONNECTION, "Failed to create SSE connection");
    }

    if (mg_url_is_ssl(client->command)) {
        struct mg_str host = mg_url_host(client->command);
        struct mg_tls_opts opts = {0};
        opts.ca = mg_str("");
        opts.name = host;
        mg_tls_init(transport->sse_conn, &opts);
    }

    struct mg_str host = mg_url_host(client->command);
    mg_printf(transport->sse_conn,
              "GET %s HTTP/1.1\r\n"
              "Host: %.*s\r\n"
              "Accept: text/event-stream\r\n"
              "Cache-Control: no-cache\r\n"
              "Connection: keep-alive\r\n",
              mg_url_uri(client->command), (int) host.len, host.buf);
    append_custom_headers(transport->sse_conn, client);
    mg_printf(transport->sse_conn, "\r\n");

    if (pthread_create(&transport->poll_thread, NULL, sse_poll_thread, client) != 0) {
        transport->running = false;
        mg_mgr_free(&transport->mgr);
        pthread_mutex_destroy(&transport->mutex);
        pthread_cond_destroy(&transport->cond);
        free(transport->read_buffer);
        free(transport->recv_buffer);
        free(transport->event_buffer);
        free(transport);
        client->transport_data = NULL;
        return error_new(ERR_CONNECTION, "Failed to start SSE poll thread");
    }
    transport->poll_thread_started = true;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += SSE_CONNECT_TIMEOUT_MS / 1000;
    ts.tv_nsec += (SSE_CONNECT_TIMEOUT_MS % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    pthread_mutex_lock(&transport->mutex);
    while (!transport->sse_open && !transport->connect_failed && transport->running) {
        int ret = pthread_cond_timedwait(&transport->cond, &transport->mutex, &ts);
        if (ret == ETIMEDOUT) break;
    }
    if (transport->sse_open && !transport->post_url) {
        struct timespec ts2;
        clock_gettime(CLOCK_REALTIME, &ts2);
        ts2.tv_sec += 5;
        while (!transport->post_url && transport->running) {
            int ret = pthread_cond_timedwait(&transport->cond, &transport->mutex, &ts2);
            if (ret == ETIMEDOUT) break;
        }
    }
    bool ready = transport->sse_open && !transport->connect_failed;
    char* err_copy = transport->error_message ? strdup(transport->error_message) : NULL;
    pthread_mutex_unlock(&transport->mutex);

    if (!ready) {
        mcp_client_disconnect(client);
        if (err_copy) {
            Error err = error_new(ERR_CONNECTION, err_copy);
            free(err_copy);
            return err;
        }
        return error_new(ERR_TIMEOUT, "SSE handshake timeout");
    }

    free(err_copy);
    mcp_sse_try_discover_post_url(client, transport);
    log_debug("[MCP sse] Connected to %s", client->server_id);
    return error_new(ERR_NONE, "");
}

static Error mcp_transport_sse_send(MCPClient* client, const char* data) {
    if (!client || !data) return error_new(ERR_INVALID_PARAM, "Invalid client or data");
    if (!client->transport_data) return error_new(ERR_INVALID_PARAM, "Invalid SSE transport");
    MCPSSETransport* transport = (MCPSSETransport*) client->transport_data;
    const char* post_url = transport->post_url;
    char fallback_url[1024];
    bool retried_with_mcp = false;
    bool retried_expired_session = false;
    if (!post_url || post_url[0] == '\0') {
        post_url = client->command;
        if (client->args_count > 0 && client->args[0] && strlen(client->args[0]) > 0) {
            post_url = client->args[0];
        }
    }
    log_debug("[MCP sse] Using POST endpoint: %s", post_url);

retry_send:
    ;
    struct mg_mgr mgr;
    MCPSSEPostContext ctx = {0};
    mg_mgr_init(&mgr);

    struct mg_connection* c = mg_http_connect(&mgr, post_url, mcp_sse_post_event_handler, &ctx);
    if (!c) {
        mg_mgr_free(&mgr);
        return error_new(ERR_CONNECTION, "Failed to create SSE POST connection");
    }

    if (mg_url_is_ssl(post_url)) {
        struct mg_str host_tls = mg_url_host(post_url);
        struct mg_tls_opts opts = {0};
        opts.ca = mg_str("");
        opts.name = host_tls;
        mg_tls_init(c, &opts);
    }

    struct mg_str host = mg_url_host(post_url);
    if (transport->session_id && transport->session_id[0] != '\0') {
        mg_printf(c,
                  "POST %s HTTP/1.0\r\n"
                  "Host: %.*s\r\n"
                  "Content-Type: application/json\r\n"
                  "Accept: application/json, text/event-stream\r\n"
                  "MCP-Protocol-Version: %s\r\n"
                  "Mcp-Session-Id: %s\r\n"
                  "Content-Length: %d\r\n",
                  mg_url_uri(post_url),
                  (int) host.len, host.buf,
                  MCP_PROTOCOL_VERSION,
                  transport->session_id,
                  (int) strlen(data));
        append_custom_headers(c, client);
        mg_printf(c, "\r\n%s", data);
    } else {
        mg_printf(c,
                  "POST %s HTTP/1.0\r\n"
                  "Host: %.*s\r\n"
                  "Content-Type: application/json\r\n"
                  "Accept: application/json, text/event-stream\r\n"
                  "MCP-Protocol-Version: %s\r\n"
                  "Content-Length: %d\r\n",
                  mg_url_uri(post_url),
                  (int) host.len, host.buf,
                  MCP_PROTOCOL_VERSION,
                  (int) strlen(data));
        append_custom_headers(c, client);
        mg_printf(c, "\r\n%s", data);
    }

    uint64_t start_ms = mg_millis();
    while (!ctx.done && (mg_millis() - start_ms) < SSE_POST_TIMEOUT_MS) {
        mg_mgr_poll(&mgr, 100);
    }
    mg_mgr_free(&mgr);

    if (!ctx.done) {
        free(ctx.response_body);
        free(ctx.session_id);
        return error_new(ERR_TIMEOUT, "SSE POST timeout");
    }
    if (!ctx.ok) {
        if (!retried_with_mcp && ctx.http_status == 405 && strstr(post_url, "/sse") != NULL) {
            const char* pos = strstr(post_url, "/sse");
            size_t prefix_len = (size_t)(pos - post_url);
            if (prefix_len + 5 < sizeof(fallback_url)) {
                memcpy(fallback_url, post_url, prefix_len);
                fallback_url[prefix_len] = '\0';
                strcat(fallback_url, "/mcp");
                log_debug("[MCP sse] Retry with fallback endpoint: %s", fallback_url);
                free(ctx.response_body);
                free(ctx.session_id);
                post_url = fallback_url;
                retried_with_mcp = true;
                goto retry_send;
            }
        }
        free(ctx.session_id);
        char parsed_msg[256];
        parsed_msg[0] = '\0';
        if (ctx.response_body && ctx.response_body[0] != '\0') {
            cJSON* err_json = cJSON_Parse(ctx.response_body);
            if (err_json) {
                cJSON* msg_item = cJSON_GetObjectItem(err_json, "Message");
                if (!cJSON_IsString(msg_item)) msg_item = cJSON_GetObjectItem(err_json, "message");
                if (cJSON_IsString(msg_item) && msg_item->valuestring && msg_item->valuestring[0] != '\0') {
                    strncpy(parsed_msg, msg_item->valuestring, sizeof(parsed_msg) - 1);
                    parsed_msg[sizeof(parsed_msg) - 1] = '\0';
                }
                cJSON_Delete(err_json);
            }
        }
        if (!retried_expired_session &&
            transport->session_id &&
            ((parsed_msg[0] != '\0' && strstr(parsed_msg, "expired") && strstr(parsed_msg, "session")) ||
             (ctx.response_body && strstr(ctx.response_body, "expired") && strstr(ctx.response_body, "session")))) {
            log_debug("[MCP sse] Session expired, retrying with a fresh session");
            free(transport->session_id);
            transport->session_id = NULL;
            retried_expired_session = true;
            free(ctx.response_body);
            goto retry_send;
        }
        free(ctx.response_body);
        if (parsed_msg[0] != '\0') {
            return error_new(ERR_CONNECTION, parsed_msg);
        }
        if (ctx.last_error[0] != '\0') {
            return error_new(ERR_CONNECTION, ctx.last_error);
        }
        char err[128];
        snprintf(err, sizeof(err), "SSE POST failed, status %d", ctx.http_status);
        return error_new(ERR_CONNECTION, err);
    }

    if (ctx.session_id && ctx.session_id[0] != '\0') {
        free(transport->session_id);
        transport->session_id = ctx.session_id;
        ctx.session_id = NULL;
    }

    if (ctx.response_body && ctx.response_body[0] != '\0') {
        cJSON* parsed = cJSON_Parse(ctx.response_body);
        if (parsed) {
            char* compact = cJSON_PrintUnformatted(parsed);
            pthread_mutex_lock(&transport->mutex);
            if (compact) {
                sse_queue_append(transport, compact);
                free(compact);
            } else {
                sse_queue_append(transport, ctx.response_body);
            }
            pthread_cond_signal(&transport->cond);
            pthread_mutex_unlock(&transport->mutex);
            cJSON_Delete(parsed);
        }
    }
    free(ctx.response_body);
    free(ctx.session_id);
    return error_new(ERR_NONE, "");
}

static char* mcp_transport_sse_recv(MCPClient* client, int timeout_ms) {
    if (!client || !client->transport_data) return NULL;
    MCPSSETransport* transport = (MCPSSETransport*) client->transport_data;

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
    char* result = sse_queue_pop(transport);
    pthread_mutex_unlock(&transport->mutex);
    return result;
}

static void mcp_transport_sse_close(MCPClient* client) {
    if (!client || !client->transport_data) return;
    MCPSSETransport* transport = (MCPSSETransport*) client->transport_data;

    pthread_mutex_lock(&transport->mutex);
    transport->running = false;
    if (transport->sse_conn) {
        transport->sse_conn->is_closing = 1;
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
    free(transport->recv_buffer);
    free(transport->event_buffer);
    free(transport->post_url);
    free(transport->session_id);
    free(transport);
    client->transport_data = NULL;
}

MCPTransportOps* mcp_transport_sse_ops(void) {
    static MCPTransportOps ops = {
        .init = mcp_transport_sse_init,
        .send = mcp_transport_sse_send,
        .recv = mcp_transport_sse_recv,
        .close = mcp_transport_sse_close
    };
    return &ops;
}
