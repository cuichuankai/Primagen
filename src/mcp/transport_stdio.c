/*
 * MCP Transport Layer - stdio implementation
 * Uses pipes to communicate with MCP servers via stdio
 */

#include "mcp.h"
#include "../include/logger.h"
#include "../vendor/cJSON/cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>

// stdio transport data
typedef struct {
    pid_t pid;              // Child process PID
    int stdin_fd;           // Write to child stdin
    int stdout_fd;          // Read from child stdout
    pthread_t reader_thread;
    bool running;
    char* read_buffer;      // Buffer for accumulating responses
    size_t buffer_size;
    size_t buffer_len;
    pthread_mutex_t mutex;  // Protects read_buffer
    pthread_cond_t cond;    // Signal when response ready
} MCPStdioTransport;

// Buffer for reading responses
#define READ_BUFFER_SIZE 65536
#define LINE_BUFFER_SIZE 4096

// Helper: Set file descriptor to non-blocking
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Reader thread - reads from child stdout and accumulates responses
static void* stdio_reader_thread(void* arg) {
    MCPClient* client = (MCPClient*)arg;
    MCPStdioTransport* transport = (MCPStdioTransport*)client->transport_data;

    char line[LINE_BUFFER_SIZE];
    size_t line_pos = 0;

    while (transport->running) {
        ssize_t n = read(transport->stdout_fd, line + line_pos, sizeof(line) - line_pos - 1);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);  // 1ms
                continue;
            }
            if (errno == EINTR) continue;
            break;  // Error or EOF
        }

        if (n == 0) {
            // EOF - child exited
            break;
        }

        line_pos += n;
        line[line_pos] = '\0';

        // Look for newline to delimit complete JSON messages
        char* nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';

            // Try to parse as JSON to validate
            cJSON* json = cJSON_Parse(line);
            if (json) {
                char* json_str = cJSON_PrintUnformatted(json);

                // Store in buffer
                pthread_mutex_lock(&transport->mutex);
                size_t needed = transport->buffer_len + strlen(json_str) + 2;
                if (needed >= transport->buffer_size) {
                    transport->buffer_size = needed * 2;
                    transport->read_buffer = realloc(transport->read_buffer, transport->buffer_size);
                }
                if (transport->buffer_len > 0) {
                    strcat(transport->read_buffer, "\n");
                    transport->buffer_len++;
                }
                strcat(transport->read_buffer, json_str);
                transport->buffer_len += strlen(json_str);
                pthread_cond_signal(&transport->cond);
                pthread_mutex_unlock(&transport->mutex);

                free(json_str);
                cJSON_Delete(json);
            }

            // Move to next line
            size_t remaining = strlen(nl + 1);
            memmove(line, nl + 1, remaining + 1);
            line_pos = remaining;
        }
    }

    transport->running = false;
    log_info("[MCP stdio] Reader thread stopped for %s", client->server_id);
    return NULL;
}

// Initialize stdio transport
static Error mcp_transport_stdio_init(MCPClient* client) {
    if (!client || !client->command) {
        return error_new(ERR_INVALID_PARAM, "Invalid client or command");
    }

    log_info("[MCP stdio] Starting %s with command: %s", client->server_id, client->command);

    // Create pipes for stdin and stdout
    int stdin_pipe[2];   // parent writes -> child reads
    int stdout_pipe[2];  // child writes -> parent reads

    if (pipe(stdin_pipe) < 0) {
        return error_new(ERR_CONNECTION, "Failed to create stdin pipe");
    }
    if (pipe(stdout_pipe) < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        return error_new(ERR_CONNECTION, "Failed to create stdout pipe");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return error_new(ERR_CONNECTION, "Failed to fork");
    }

    if (pid == 0) {
        // Child process
        close(stdin_pipe[1]);    // Close write end of stdin
        close(stdout_pipe[0]);   // Close read end of stdout

        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        // Set environment variables
        for (size_t i = 0; i < client->env.count; i++) {
            setenv(client->env.items[i].key, client->env.items[i].value, 1);
        }

        // Build argv
        size_t argc = client->args_count + 1;
        char** argv = malloc((argc + 1) * sizeof(char*));
        argv[0] = client->command;
        for (size_t i = 0; i < client->args_count; i++) {
            argv[i + 1] = client->args[i];
        }
        argv[argc] = NULL;

        // Execute
        execvp(client->command, argv);

        // If exec fails
        perror("execvp failed");
        _exit(127);
    }

    // Parent process
    close(stdin_pipe[0]);    // Close read end
    close(stdout_pipe[1]);   // Close write end

    // Set non-blocking for reading
    set_nonblocking(stdout_pipe[0]);

    // Create transport data
    MCPStdioTransport* transport = calloc(1, sizeof(MCPStdioTransport));
    transport->pid = pid;
    transport->stdin_fd = stdin_pipe[1];
    transport->stdout_fd = stdout_pipe[0];
    transport->running = true;
    transport->read_buffer = malloc(READ_BUFFER_SIZE);
    transport->read_buffer[0] = '\0';
    transport->buffer_size = READ_BUFFER_SIZE;
    transport->buffer_len = 0;
    pthread_mutex_init(&transport->mutex, NULL);
    pthread_cond_init(&transport->cond, NULL);

    client->transport_data = transport;

    // Start reader thread
    if (pthread_create(&transport->reader_thread, NULL, stdio_reader_thread, client) != 0) {
        log_error("[MCP stdio] Failed to create reader thread");
        close(transport->stdin_fd);
        close(transport->stdout_fd);
        free(transport->read_buffer);
        free(transport);
        client->transport_data = NULL;
        return error_new(ERR_CONNECTION, "Failed to create reader thread");
    }

    log_info("[MCP stdio] Started process %d for %s", pid, client->server_id);
    return error_new(ERR_NONE, "");
}

// Send data via stdio transport
static Error mcp_transport_stdio_send(MCPClient* client, const char* data) {
    if (!client || !client->transport_data) {
        return error_new(ERR_INVALID_PARAM, "Invalid client or transport");
    }

    MCPStdioTransport* transport = (MCPStdioTransport*)client->transport_data;

    if (!transport->running) {
        return error_new(ERR_CONNECTION, "Transport not running");
    }

    size_t len = strlen(data);
    ssize_t written = write(transport->stdin_fd, data, len);

    if (written < 0) {
        return error_new(ERR_CONNECTION, "Failed to write to stdin");
    }

    // Write newline to flush
    write(transport->stdin_fd, "\n", 1);

    log_debug("[MCP stdio] Sent %zd bytes to %s", written, client->server_id);
    return error_new(ERR_NONE, "");
}

// Receive data from stdio transport (with timeout in ms)
static char* mcp_transport_stdio_recv(MCPClient* client, int timeout_ms) {
    if (!client || !client->transport_data) {
        return NULL;
    }

    MCPStdioTransport* transport = (MCPStdioTransport*)client->transport_data;

    if (!transport->running) {
        return NULL;
    }

    char* result = NULL;

    // Wait for data with timeout
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    pthread_mutex_lock(&transport->mutex);

    // Wait for data
    while (transport->buffer_len == 0 && transport->running) {
        int ret = pthread_cond_timedwait(&transport->cond, &transport->mutex, &ts);
        if (ret == ETIMEDOUT) {
            break;
        }
    }

    if (transport->buffer_len > 0) {
        result = strdup(transport->read_buffer);
        transport->read_buffer[0] = '\0';
        transport->buffer_len = 0;
    }

    pthread_mutex_unlock(&transport->mutex);

    return result;
}

// Close stdio transport
static void mcp_transport_stdio_close(MCPClient* client) {
    if (!client || !client->transport_data) {
        return;
    }

    MCPStdioTransport* transport = (MCPStdioTransport*)client->transport_data;

    transport->running = false;

    // Wait for reader thread
    pthread_join(transport->reader_thread, NULL);

    // Close pipes
    close(transport->stdin_fd);
    close(transport->stdout_fd);

    // Kill child process
    kill(transport->pid, SIGTERM);
    waitpid(transport->pid, NULL, 0);

    // Free resources
    pthread_mutex_destroy(&transport->mutex);
    pthread_cond_destroy(&transport->cond);
    free(transport->read_buffer);
    free(transport);

    client->transport_data = NULL;
    log_info("[MCP stdio] Closed connection to %s", client->server_id);
}

// Transport vtable
typedef struct {
    Error (*init)(MCPClient* client);
    Error (*send)(MCPClient* client, const char* data);
    char* (*recv)(MCPClient* client, int timeout_ms);
    void (*close)(MCPClient* client);
} MCPTransportOps;

static MCPTransportOps stdio_ops = {
    .init = mcp_transport_stdio_init,
    .send = mcp_transport_stdio_send,
    .recv = mcp_transport_stdio_recv,
    .close = mcp_transport_stdio_close
};

// Get transport ops by type
static MCPTransportOps* get_transport_ops(const char* type) {
    if (strcmp(type, "stdio") == 0) {
        return &stdio_ops;
    }
    // Add other transports here
    return NULL;
}

// Global transport operations table - used for future transport types

// Initialize transport for client
Error mcp_client_connect(MCPClient* client) {
    if (!client) return error_new(ERR_INVALID_PARAM, "client is NULL");

    log_info("[MCP] Connecting to %s via %s...", client->server_id, client->transport_type);

    MCPTransportOps* ops = get_transport_ops(client->transport_type);
    if (!ops) {
        log_error("[MCP] Unknown transport type: %s", client->transport_type);
        return error_new(ERR_INVALID_PARAM, "Unknown transport type");
    }

    Error err = ops->init(client);
    if (err.code == ERR_NONE) {
        client->connected = true;
        log_info("[MCP] Connected to %s", client->server_id);
    }

    return err;
}

void mcp_client_disconnect(MCPClient* client) {
    if (!client) return;

    log_info("[MCP] Disconnecting from %s", client->server_id);

    MCPTransportOps* ops = get_transport_ops(client->transport_type);
    if (ops && ops->close) {
        ops->close(client);
    }

    client->connected = false;
}

// Send request and wait for response
Error mcp_client_send(MCPClient* client, const char* request_json) {
    if (!client || !request_json) {
        return error_new(ERR_INVALID_PARAM, "Invalid arguments");
    }

    MCPTransportOps* ops = get_transport_ops(client->transport_type);
    if (!ops || !ops->send) {
        return error_new(ERR_INVALID_PARAM, "Transport not available");
    }

    return ops->send(client, request_json);
}

// Receive response with timeout
char* mcp_client_recv(MCPClient* client, int timeout_ms) {
    if (!client) return NULL;

    MCPTransportOps* ops = get_transport_ops(client->transport_type);
    if (!ops || !ops->recv) {
        return NULL;
    }

    return ops->recv(client, timeout_ms);
}

// Send request and receive response (convenience function)
// Note: Currently implemented inline in MCP method functions
