#ifndef MCP_TRANSPORT_INTERNAL_H
#define MCP_TRANSPORT_INTERNAL_H

#include "mcp.h"
#include "../../../src/vendor/mongoose/mongoose.h"
#include <strings.h>
#include <string.h>

typedef struct {
    Error (*init)(MCPClient* client);
    Error (*send)(MCPClient* client, const char* data);
    char* (*recv)(MCPClient* client, int timeout_ms);
    void (*close)(MCPClient* client);
} MCPTransportOps;

MCPTransportOps* mcp_transport_stdio_ops(void);
MCPTransportOps* mcp_transport_websocket_ops(void);
MCPTransportOps* mcp_transport_sse_ops(void);
MCPTransportOps* mcp_transport_streamable_http_ops(void);

static inline const char* get_header_value_ci(struct mg_http_message* hm, const char* header_name, char* buf, size_t buf_size) {
    if (!hm || !header_name || !buf || buf_size == 0) return NULL;
    size_t target_len = strlen(header_name);
    for (int i = 0; i < MG_MAX_HTTP_HEADERS; i++) {
        struct mg_str name = hm->headers[i].name;
        struct mg_str value = hm->headers[i].value;
        if (name.len == 0) continue;
        if (name.len == target_len && strncasecmp(name.buf, header_name, target_len) == 0) {
            size_t n = value.len < buf_size - 1 ? value.len : buf_size - 1;
            memcpy(buf, value.buf, n);
            buf[n] = '\0';
            return buf;
        }
    }
    return NULL;
}

#endif
