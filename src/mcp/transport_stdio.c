/*
 * MCP Transport Layer - stdio implementation
 * Uses pipes to communicate with MCP servers via stdio
 */

#include "mcp.h"
#include "transport_internal.h"
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
#include <stdint.h>

typedef struct {
    pid_t pid;
    int stdin_fd;
    int stdout_fd;
    pthread_t reader_thread;
    bool running;
    char* read_buffer;
    size_t buffer_size;
    size_t buffer_len;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int reconnect_attempts;
    time_t last_disconnect;
    bool reconnecting;
} MCPStdioTransport;

#define MAX_RECONNECT_ATTEMPTS 5
#define RECONNECT_BASE_DELAY_MS 1000
#define READ_BUFFER_SIZE 65536
#define LINE_BUFFER_SIZE 4096

static void* stdio_reader_thread(void* arg);

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void sleep_ms(int ms) {
    usleep(ms * 1000);
}

static Error mcp_transport_stdio_reconnect(MCPClient* client) {
    if (!client || !client->transport_data) {
        return error_new(ERR_INVALID_PARAM, "Invalid client or transport");
    }

    MCPStdioTransport* transport = (MCPStdioTransport*)client->transport_data;
    if (transport->reconnecting) {
        log_debug("[MCP stdio] Reconnection already in progress for %s", client->server_id);
        return error_new(ERR_CONNECTION, "Reconnection already in progress");
    }

    transport->reconnecting = true;
    log_debug("[MCP stdio] Attempting reconnection for %s (attempt %d/%d)",
              client->server_id, transport->reconnect_attempts, MAX_RECONNECT_ATTEMPTS);

    if (transport->stdin_fd >= 0) close(transport->stdin_fd);
    if (transport->stdout_fd >= 0) close(transport->stdout_fd);

    if (!pthread_equal(pthread_self(), transport->reader_thread)) {
        pthread_join(transport->reader_thread, NULL);
    }

    int stdin_pipe[2];
    int stdout_pipe[2];
    if (pipe(stdin_pipe) < 0) {
        transport->reconnecting = false;
        return error_new(ERR_CONNECTION, "Failed to create stdin pipe");
    }
    if (pipe(stdout_pipe) < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        transport->reconnecting = false;
        return error_new(ERR_CONNECTION, "Failed to create stdout pipe");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        transport->reconnecting = false;
        return error_new(ERR_CONNECTION, "Failed to fork");
    }

    if (pid == 0) {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        for (size_t i = 0; i < client->env.count; i++) {
            setenv(client->env.items[i].key, client->env.items[i].value, 1);
        }
        size_t argc = client->args_count + 1;
        char** argv = malloc((argc + 1) * sizeof(char*));
        if (!argv) _exit(127);
        argv[0] = client->command;
        for (size_t i = 0; i < client->args_count; i++) argv[i + 1] = client->args[i];
        argv[argc] = NULL;
        execvp(client->command, argv);
        perror("execvp failed");
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    set_nonblocking(stdout_pipe[0]);

    transport->pid = pid;
    transport->stdin_fd = stdin_pipe[1];
    transport->stdout_fd = stdout_pipe[0];
    transport->running = true;
    transport->read_buffer[0] = '\0';
    transport->buffer_len = 0;

    if (pthread_create(&transport->reader_thread, NULL, stdio_reader_thread, client) != 0) {
        log_error("[MCP stdio] Failed to create reader thread for reconnect");
        close(transport->stdin_fd);
        close(transport->stdout_fd);
        transport->running = false;
        transport->reconnecting = false;
        return error_new(ERR_CONNECTION, "Failed to create reader thread");
    }

    transport->reconnecting = false;
    log_debug("[MCP stdio] Reconnected process %d for %s", pid, client->server_id);
    return error_new(ERR_NONE, "");
}

static void* stdio_reader_thread(void* arg) {
    MCPClient* client = (MCPClient*)arg;
    MCPStdioTransport* transport = (MCPStdioTransport*)client->transport_data;
    char line[LINE_BUFFER_SIZE];
    size_t line_pos = 0;

    while (transport->running) {
        ssize_t n = read(transport->stdout_fd, line + line_pos, sizeof(line) - line_pos - 1);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            if (errno == EINTR) continue;
            log_error("[MCP stdio] Read error for %s: %s", client->server_id, strerror(errno));
            transport->last_disconnect = time(NULL);
            break;
        }
        if (n == 0) {
            log_debug("[MCP stdio] EOF received for %s", client->server_id);
            transport->last_disconnect = time(NULL);
            break;
        }

        line_pos += n;
        line[line_pos] = '\0';
        char* nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
            cJSON* json = cJSON_Parse(line);
            if (json) {
                char* json_str = cJSON_PrintUnformatted(json);
                if (!json_str) {
                    cJSON_Delete(json);
                    size_t remaining = strlen(nl + 1);
                    memmove(line, nl + 1, remaining + 1);
                    line_pos = remaining;
                    continue;
                }
                pthread_mutex_lock(&transport->mutex);
                size_t json_len = strlen(json_str);
                if (json_len > SIZE_MAX - transport->buffer_len - 2) {
                    pthread_mutex_unlock(&transport->mutex);
                    free(json_str);
                    cJSON_Delete(json);
                    size_t remaining = strlen(nl + 1);
                    memmove(line, nl + 1, remaining + 1);
                    line_pos = remaining;
                    continue;
                }
                size_t needed = transport->buffer_len + json_len + 2;
                if (needed >= transport->buffer_size) {
                    if (needed > SIZE_MAX / 2) {
                        pthread_mutex_unlock(&transport->mutex);
                        free(json_str);
                        cJSON_Delete(json);
                        size_t remaining = strlen(nl + 1);
                        memmove(line, nl + 1, remaining + 1);
                        line_pos = remaining;
                        continue;
                    }
                    size_t new_size = needed * 2;
                    char* new_buf = realloc(transport->read_buffer, new_size);
                    if (!new_buf) {
                        pthread_mutex_unlock(&transport->mutex);
                        free(json_str);
                        cJSON_Delete(json);
                        size_t remaining = strlen(nl + 1);
                        memmove(line, nl + 1, remaining + 1);
                        line_pos = remaining;
                        continue;
                    }
                    transport->read_buffer = new_buf;
                    transport->buffer_size = new_size;
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
            size_t remaining = strlen(nl + 1);
            memmove(line, nl + 1, remaining + 1);
            line_pos = remaining;
        }
    }

    if (transport->running && !transport->reconnecting) {
        transport->reconnect_attempts++;
        if (transport->reconnect_attempts <= MAX_RECONNECT_ATTEMPTS) {
            int delay_ms = RECONNECT_BASE_DELAY_MS * (1 << transport->reconnect_attempts);
            log_debug("[MCP stdio] Waiting %dms before reconnect attempt %d for %s",
                      delay_ms, transport->reconnect_attempts, client->server_id);
            sleep_ms(delay_ms);
            Error err = mcp_transport_stdio_reconnect(client);
            if (err.code == ERR_NONE) {
                log_debug("[MCP stdio] Reconnection successful for %s", client->server_id);
                transport->reconnect_attempts = 0;
                return NULL;
            } else {
                log_error("[MCP stdio] Reconnection failed for %s: %s", client->server_id, err.message);
            }
        } else {
            log_error("[MCP stdio] Max reconnection attempts reached for %s after %d attempts",
                      client->server_id, transport->reconnect_attempts);
        }
    }

    transport->running = false;
    log_debug("[MCP stdio] Reader thread stopped for %s", client->server_id);
    return NULL;
}

static Error mcp_transport_stdio_init(MCPClient* client) {
    if (!client || !client->command) {
        return error_new(ERR_INVALID_PARAM, "Invalid client or command");
    }

    log_debug("[MCP stdio] Starting %s with command: %s", client->server_id, client->command);

    int stdin_pipe[2];
    int stdout_pipe[2];
    if (pipe(stdin_pipe) < 0) return error_new(ERR_CONNECTION, "Failed to create stdin pipe");
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
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        for (size_t i = 0; i < client->env.count; i++) {
            setenv(client->env.items[i].key, client->env.items[i].value, 1);
        }
        size_t argc = client->args_count + 1;
        char** argv = malloc((argc + 1) * sizeof(char*));
        if (!argv) _exit(127);
        argv[0] = client->command;
        for (size_t i = 0; i < client->args_count; i++) argv[i + 1] = client->args[i];
        argv[argc] = NULL;
        execvp(client->command, argv);
        perror("execvp failed");
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    set_nonblocking(stdout_pipe[0]);

    MCPStdioTransport* transport = calloc(1, sizeof(MCPStdioTransport));
    if (!transport) {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return error_new(ERR_MEMORY, "Failed to allocate stdio transport");
    }
    transport->pid = pid;
    transport->stdin_fd = stdin_pipe[1];
    transport->stdout_fd = stdout_pipe[0];
    transport->running = true;
    transport->read_buffer = malloc(READ_BUFFER_SIZE);
    if (!transport->read_buffer) {
        close(transport->stdin_fd);
        close(transport->stdout_fd);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        free(transport);
        return error_new(ERR_MEMORY, "Failed to allocate stdio buffer");
    }
    transport->read_buffer[0] = '\0';
    transport->buffer_size = READ_BUFFER_SIZE;
    transport->buffer_len = 0;
    pthread_mutex_init(&transport->mutex, NULL);
    pthread_cond_init(&transport->cond, NULL);
    client->transport_data = transport;

    if (pthread_create(&transport->reader_thread, NULL, stdio_reader_thread, client) != 0) {
        log_error("[MCP stdio] Failed to create reader thread");
        close(transport->stdin_fd);
        close(transport->stdout_fd);
        free(transport->read_buffer);
        free(transport);
        client->transport_data = NULL;
        return error_new(ERR_CONNECTION, "Failed to create reader thread");
    }

    log_debug("[MCP stdio] Started process %d for %s", pid, client->server_id);
    return error_new(ERR_NONE, "");
}

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
    write(transport->stdin_fd, "\n", 1);
    log_debug("[MCP stdio] Sent %zd bytes to %s", written, client->server_id);
    return error_new(ERR_NONE, "");
}

static char* mcp_transport_stdio_recv(MCPClient* client, int timeout_ms) {
    if (!client || !client->transport_data) return NULL;
    MCPStdioTransport* transport = (MCPStdioTransport*)client->transport_data;
    if (!transport->running) return NULL;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    char* result = NULL;
    pthread_mutex_lock(&transport->mutex);
    while (transport->buffer_len == 0 && transport->running) {
        int ret = pthread_cond_timedwait(&transport->cond, &transport->mutex, &ts);
        if (ret == ETIMEDOUT) break;
    }
    if (transport->buffer_len > 0) {
        result = strdup(transport->read_buffer);
        transport->read_buffer[0] = '\0';
        transport->buffer_len = 0;
    }
    pthread_mutex_unlock(&transport->mutex);
    return result;
}

static void mcp_transport_stdio_close(MCPClient* client) {
    if (!client || !client->transport_data) return;
    MCPStdioTransport* transport = (MCPStdioTransport*)client->transport_data;
    transport->running = false;
    pthread_join(transport->reader_thread, NULL);
    close(transport->stdin_fd);
    close(transport->stdout_fd);
    kill(transport->pid, SIGTERM);
    waitpid(transport->pid, NULL, 0);
    pthread_mutex_destroy(&transport->mutex);
    pthread_cond_destroy(&transport->cond);
    free(transport->read_buffer);
    free(transport);
    client->transport_data = NULL;
    log_debug("[MCP stdio] Closed connection to %s", client->server_id);
}

MCPTransportOps* mcp_transport_stdio_ops(void) {
    static MCPTransportOps ops = {
        .init = mcp_transport_stdio_init,
        .send = mcp_transport_stdio_send,
        .recv = mcp_transport_stdio_recv,
        .close = mcp_transport_stdio_close
    };
    return &ops;
}
