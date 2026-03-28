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
    char* read_buffer;
    size_t buffer_size;
    size_t buffer_len;
    char* session_id;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} MCPStreamableHttpTransport;

typedef struct {
    bool done;
    bool ok;
    int http_status;
    char last_error[256];
    char* response_body;
    char* session_id;
    char content_type[128];
} MCPStreamableHttpPostContext;

#define STREAMABLE_HTTP_BUFFER_SIZE 65536
#define STREAMABLE_HTTP_POST_TIMEOUT_MS 30000

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

static const char* get_header_value_ci(struct mg_http_message* hm, const char* header_name) {
    if (!hm || !header_name) return NULL;
    size_t target_len = strlen(header_name);
    for (int i = 0; i < MG_MAX_HTTP_HEADERS; i++) {
        struct mg_str name = hm->headers[i].name;
        struct mg_str value = hm->headers[i].value;
        if (name.len == 0) continue;
        if (name.len == target_len && strncasecmp(name.buf, header_name, target_len) == 0) {
            static char buf[1024];
            size_t n = value.len < sizeof(buf) - 1 ? value.len : sizeof(buf) - 1;
            memcpy(buf, value.buf, n);
            buf[n] = '\0';
            return buf;
        }
    }
    return NULL;
}

static void streamable_http_queue_append(MCPStreamableHttpTransport* transport, const char* msg) {
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

static char* streamable_http_queue_pop(MCPStreamableHttpTransport* transport) {
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

static bool contains_substring(const char* str, const char* sub) {
    if (!str || !sub) return false;
    return strstr(str, sub) != NULL;
}

static void streamable_http_enqueue_json(MCPStreamableHttpTransport* transport, const char* json_text) {
    if (!transport || !json_text || json_text[0] == '\0') return;
    cJSON* parsed = cJSON_Parse(json_text);
    if (!parsed) return;
    char* compact = cJSON_PrintUnformatted(parsed);
    pthread_mutex_lock(&transport->mutex);
    if (compact) {
        streamable_http_queue_append(transport, compact);
        free(compact);
    } else {
        streamable_http_queue_append(transport, json_text);
    }
    pthread_cond_signal(&transport->cond);
    pthread_mutex_unlock(&transport->mutex);
    cJSON_Delete(parsed);
}

static void streamable_http_enqueue_sse(MCPStreamableHttpTransport* transport, const char* body) {
    if (!transport || !body) return;
    char* work = strdup(body);
    if (!work) return;

    char* event_data = calloc(1, 1);
    size_t event_len = 0;
    char* saveptr = NULL;
    char* line = strtok_r(work, "\n", &saveptr);

    while (line) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\r') line[len - 1] = '\0';

        if (line[0] == '\0') {
            if (event_len > 0) {
                while (event_len > 0 && (event_data[event_len - 1] == '\n' || event_data[event_len - 1] == '\r')) {
                    event_data[--event_len] = '\0';
                }
                if (strcmp(event_data, "[DONE]") != 0) {
                    streamable_http_enqueue_json(transport, event_data);
                }
                event_len = 0;
                event_data[0] = '\0';
            }
        } else if (strncmp(line, "data:", 5) == 0) {
            const char* payload = line + 5;
            if (*payload == ' ') payload++;
            size_t p_len = strlen(payload);
            char* new_data = realloc(event_data, event_len + p_len + 2);
            if (!new_data) break;
            event_data = new_data;
            memcpy(event_data + event_len, payload, p_len);
            event_len += p_len;
            event_data[event_len++] = '\n';
            event_data[event_len] = '\0';
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    if (event_len > 0) {
        while (event_len > 0 && (event_data[event_len - 1] == '\n' || event_data[event_len - 1] == '\r')) {
            event_data[--event_len] = '\0';
        }
        if (event_len > 0 && strcmp(event_data, "[DONE]") != 0) {
            streamable_http_enqueue_json(transport, event_data);
        }
    }

    free(event_data);
    free(work);
}

static void mcp_streamable_http_post_event_handler(struct mg_connection* c, int ev, void* ev_data) {
    MCPStreamableHttpPostContext* ctx = (MCPStreamableHttpPostContext*) c->fn_data;
    if (!ctx) return;

    if (ev == MG_EV_HTTP_HDRS) {
        struct mg_http_message* hm = (struct mg_http_message*) ev_data;
        ctx->http_status = mg_http_status(hm);
        const char* sid = get_header_value_ci(hm, "mcp-session-id");
        if (sid && sid[0] != '\0') {
            size_t sid_len = strlen(sid);
            ctx->session_id = malloc(sid_len + 1);
            if (ctx->session_id) {
                memcpy(ctx->session_id, sid, sid_len);
                ctx->session_id[sid_len] = '\0';
            }
        }

        const char* ct = get_header_value_ci(hm, "content-type");
        if (ct && ct[0] != '\0') {
            size_t ct_len = strlen(ct);
            size_t n = ct_len < sizeof(ctx->content_type) - 1 ? ct_len : sizeof(ctx->content_type) - 1;
            memcpy(ctx->content_type, ct, n);
            ctx->content_type[n] = '\0';
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
        const char* err = ev_data ? (const char*) ev_data : "Streamable HTTP POST error";
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

static Error mcp_transport_streamable_http_init(MCPClient* client) {
    if (!client || !client->command || strlen(client->command) == 0) {
        return error_new(ERR_INVALID_PARAM, "Invalid streamable_http URL");
    }
    MCPStreamableHttpTransport* transport = calloc(1, sizeof(MCPStreamableHttpTransport));
    if (!transport) return error_new(ERR_MEMORY, "Failed to allocate streamable_http transport");
    transport->buffer_size = STREAMABLE_HTTP_BUFFER_SIZE;
    transport->read_buffer = malloc(transport->buffer_size);
    if (!transport->read_buffer) {
        free(transport);
        return error_new(ERR_MEMORY, "Failed to allocate streamable_http buffer");
    }
    transport->read_buffer[0] = '\0';
    pthread_mutex_init(&transport->mutex, NULL);
    pthread_cond_init(&transport->cond, NULL);
    client->transport_data = transport;
    return error_new(ERR_NONE, "");
}

static Error mcp_transport_streamable_http_send(MCPClient* client, const char* data) {
    if (!client || !data) return error_new(ERR_INVALID_PARAM, "Invalid client or data");
    if (!client->transport_data) return error_new(ERR_INVALID_PARAM, "Invalid streamable_http transport");
    MCPStreamableHttpTransport* transport = (MCPStreamableHttpTransport*) client->transport_data;

    const char* post_url = client->command;
    if (client->args_count > 0 && client->args[0] && strlen(client->args[0]) > 0) {
        post_url = client->args[0];
    }
    bool retried_expired_session = false;

retry_send:
    ;
    struct mg_mgr mgr;
    MCPStreamableHttpPostContext ctx = {0};
    mg_mgr_init(&mgr);
    struct mg_connection* c = mg_http_connect(&mgr, post_url, mcp_streamable_http_post_event_handler, &ctx);
    if (!c) {
        mg_mgr_free(&mgr);
        return error_new(ERR_CONNECTION, "Failed to create streamable_http connection");
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
                  "POST %s HTTP/1.1\r\n"
                  "Host: %.*s\r\n"
                  "Content-Type: application/json\r\n"
                  "Accept: application/json, text/event-stream\r\n"
                  "MCP-Protocol-Version: %s\r\n"
                  "MCP-Session-Id: %s\r\n"
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
                  "POST %s HTTP/1.1\r\n"
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
    while (!ctx.done && (mg_millis() - start_ms) < STREAMABLE_HTTP_POST_TIMEOUT_MS) {
        mg_mgr_poll(&mgr, 100);
    }
    mg_mgr_free(&mgr);

    if (!ctx.done) {
        free(ctx.response_body);
        free(ctx.session_id);
        return error_new(ERR_TIMEOUT, "Streamable HTTP timeout");
    }
    if (!ctx.ok) {
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
            ((parsed_msg[0] != '\0' && contains_substring(parsed_msg, "expired") && contains_substring(parsed_msg, "session")) ||
             (ctx.response_body && contains_substring(ctx.response_body, "expired") && contains_substring(ctx.response_body, "session")))) {
            log_debug("[MCP streamable_http] Session expired, retrying with a fresh session");
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
        if (ctx.last_error[0] != '\0') return error_new(ERR_CONNECTION, ctx.last_error);
        char err[128];
        snprintf(err, sizeof(err), "Streamable HTTP failed, status %d", ctx.http_status);
        return error_new(ERR_CONNECTION, err);
    }

    if (ctx.session_id && ctx.session_id[0] != '\0') {
        free(transport->session_id);
        transport->session_id = ctx.session_id;
        ctx.session_id = NULL;
    }

    if (ctx.response_body && ctx.response_body[0] != '\0') {
        if (contains_substring(ctx.content_type, "text/event-stream")) {
            streamable_http_enqueue_sse(transport, ctx.response_body);
        } else {
            streamable_http_enqueue_json(transport, ctx.response_body);
        }
    }

    free(ctx.response_body);
    free(ctx.session_id);
    return error_new(ERR_NONE, "");
}

static char* mcp_transport_streamable_http_recv(MCPClient* client, int timeout_ms) {
    if (!client || !client->transport_data) return NULL;
    MCPStreamableHttpTransport* transport = (MCPStreamableHttpTransport*) client->transport_data;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    pthread_mutex_lock(&transport->mutex);
    while (transport->buffer_len == 0) {
        int ret = pthread_cond_timedwait(&transport->cond, &transport->mutex, &ts);
        if (ret == ETIMEDOUT) break;
    }
    char* result = streamable_http_queue_pop(transport);
    pthread_mutex_unlock(&transport->mutex);
    return result;
}

static void mcp_transport_streamable_http_close(MCPClient* client) {
    if (!client || !client->transport_data) return;
    MCPStreamableHttpTransport* transport = (MCPStreamableHttpTransport*) client->transport_data;
    pthread_mutex_destroy(&transport->mutex);
    pthread_cond_destroy(&transport->cond);
    free(transport->session_id);
    free(transport->read_buffer);
    free(transport);
    client->transport_data = NULL;
}

MCPTransportOps* mcp_transport_streamable_http_ops(void) {
    static MCPTransportOps ops = {
        .init = mcp_transport_streamable_http_init,
        .send = mcp_transport_streamable_http_send,
        .recv = mcp_transport_streamable_http_recv,
        .close = mcp_transport_streamable_http_close
    };
    return &ops;
}
