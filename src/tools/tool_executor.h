#ifndef TOOL_EXECUTOR_H
#define TOOL_EXECUTOR_H

#include "../include/common.h"
#include "tool.h"
#include <pthread.h>

// =============================================================================
// Tool Executor Types
// =============================================================================

// Tool execution request
typedef struct ToolExecutionRequest {
    char* tool_name;
    char* arguments;
    void* context;  // User data for callback
    void (*callback)(void* context, const char* tool_name, const char* result, Error err);
    struct ToolExecutionRequest* next;
} ToolExecutionRequest;

typedef struct {
    pthread_t thread;
    struct ToolExecutor* executor;
    int worker_id;
} ToolExecutorWorker;

// Tool executor with thread pool
typedef struct ToolExecutor {
    // Request queue
    ToolExecutionRequest* request_queue;
    ToolExecutionRequest* request_queue_tail;
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;

    // Workers
    ToolExecutorWorker* workers;
    size_t num_workers;

    // Shutdown flag
    bool shutdown;

    // Tool registry reference
    ToolRegistry* tool_reg;
} ToolExecutor;

// =============================================================================
// Tool Executor Functions
// =============================================================================

/**
 * Create a new tool executor with thread pool
 * @param tool_reg The tool registry to use for execution
 * @param num_workers Number of worker threads (0 = auto-detect)
 * @return New tool executor or NULL on failure
 */
ToolExecutor* tool_executor_new(ToolRegistry* tool_reg, size_t num_workers);

/**
 * Destroy tool executor and free all resources
 * @param executor The tool executor to destroy
 */
void tool_executor_destroy(ToolExecutor* executor);

/**
 * Submit a tool execution request (non-blocking)
 * @param executor The tool executor
 * @param tool_name Name of the tool to execute
 * @param arguments JSON arguments for the tool
 * @param context User context for callback
 * @param callback Callback function when execution completes
 * @return 0 on success, -1 on failure
 */
int tool_executor_submit(ToolExecutor* executor, const char* tool_name,
                         const char* arguments, void* context,
                         void (*callback)(void* context, const char* tool_name, const char* result, Error err));

typedef void (*ToolAsyncCallback)(Error err, const char* result, void* user_data);
void tool_executor_submit_async(ToolExecutor* executor, const char* tool_name, const char* args_json, ToolAsyncCallback callback, void* user_data);

/**
 * Execute a tool synchronously with timeout (blocking)
 * @param executor The tool executor
 * @param tool_name Name of the tool to execute
 * @param arguments JSON arguments for the tool
 * @param result Output string for result
 * @param timeout_ms Timeout in milliseconds (0 = no timeout)
 * @return Error code
 */
Error tool_executor_execute_sync(ToolExecutor* executor, const char* tool_name,
                                  const char* arguments, String* result, int timeout_ms);

#endif // TOOL_EXECUTOR_H
