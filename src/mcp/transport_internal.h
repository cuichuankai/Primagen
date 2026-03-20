#ifndef MCP_TRANSPORT_INTERNAL_H
#define MCP_TRANSPORT_INTERNAL_H

#include "mcp.h"

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

#endif
