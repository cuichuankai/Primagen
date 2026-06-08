#include "tool_executor.h"
#include "../include/logger.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <errno.h>

// =============================================================================
// Internal Helper Functions
// =============================================================================

// Worker thread function
static void* tool_executor_worker(void* arg) {
    ToolExecutorWorker* worker = (ToolExecutorWorker*)arg;
    ToolExecutor* executor = worker->executor;

    log_debug("[ToolExecutor] Worker %d started", worker->worker_id);

    while (1) {
        pthread_mutex_lock(&executor->queue_mutex);

        // Wait for work or shutdown
        while (executor->request_queue == NULL && !executor->shutdown) {
            pthread_cond_wait(&executor->queue_cond, &executor->queue_mutex);
        }

        if (executor->shutdown && executor->request_queue == NULL) {
            pthread_mutex_unlock(&executor->queue_mutex);
            break;
        }

        // Dequeue request
        ToolExecutionRequest* request = executor->request_queue;
        if (request) {
            executor->request_queue = request->next;
            if (executor->request_queue == NULL) {
                executor->request_queue_tail = NULL;
            }
        }

        pthread_mutex_unlock(&executor->queue_mutex);

        if (!request) continue;

        // Execute the tool
        String result = string_new("");
        Error err = tool_registry_execute_with_user_data(
            executor->tool_reg,
            request->tool_name,
            request->arguments,
            request->tool_user_data_override,
            &result
        );

        // Call callback
        if (request->callback) {
            request->callback(request->context, request->tool_name, result.data, err);
        }

        // Cleanup
        string_free(&result);
        if (request->tool_user_data_override && request->tool_user_data_destroy) {
            request->tool_user_data_destroy(request->tool_user_data_override);
        }
        free(request->tool_name);
        free(request->arguments);
        free(request);
    }

    log_debug("[ToolExecutor] Worker %d stopped", worker->worker_id);
    return NULL;
}

// =============================================================================
// Tool Executor Implementation
// =============================================================================

ToolExecutor* tool_executor_new(ToolRegistry* tool_reg, size_t num_workers) {
    ToolExecutor* executor = calloc(1, sizeof(ToolExecutor));
    if (!executor) return NULL;

    executor->tool_reg = tool_reg;
    executor->shutdown = false;

    // Auto-detect number of workers
    if (num_workers == 0) {
        num_workers = 2;
    }

    pthread_mutex_init(&executor->queue_mutex, NULL);
    pthread_cond_init(&executor->queue_cond, NULL);

    executor->num_workers = num_workers;
    executor->workers = malloc(num_workers * sizeof(ToolExecutorWorker));
    if (!executor->workers) {
        free(executor);
        return NULL;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 32 * 1024);

    for (size_t i = 0; i < num_workers; i++) {
        executor->workers[i].executor = executor;
        executor->workers[i].worker_id = i;
        pthread_create(&executor->workers[i].thread, &attr,
                       tool_executor_worker, &executor->workers[i]);
    }

    pthread_attr_destroy(&attr);

    log_debug("[ToolExecutor] Created with %zu workers", num_workers);
    return executor;
}

void tool_executor_destroy(ToolExecutor* executor) {
    if (!executor) return;

    log_debug("[ToolExecutor] Shutting down...");

    // Signal shutdown
    pthread_mutex_lock(&executor->queue_mutex);
    executor->shutdown = true;
    pthread_cond_broadcast(&executor->queue_cond);
    pthread_mutex_unlock(&executor->queue_mutex);

    // Wait for workers to finish
    for (size_t i = 0; i < executor->num_workers; i++) {
        pthread_join(executor->workers[i].thread, NULL);
    }

    // Cleanup queue
    ToolExecutionRequest* request = executor->request_queue;
    while (request) {
        ToolExecutionRequest* next = request->next;
        if (request->tool_user_data_override && request->tool_user_data_destroy) {
            request->tool_user_data_destroy(request->tool_user_data_override);
        }
        free(request->tool_name);
        free(request->arguments);
        free(request);
        request = next;
    }

    // Destroy synchronization primitives
    pthread_mutex_destroy(&executor->queue_mutex);
    pthread_cond_destroy(&executor->queue_cond);

    free(executor->workers);
    free(executor);
    log_debug("[ToolExecutor] Shutdown complete");
}

int tool_executor_submit(ToolExecutor* executor, const char* tool_name,
                         const char* arguments, void* context,
                         void (*callback)(void* context, const char* tool_name, const char* result, Error err)) {
    return tool_executor_submit_with_user_data(executor, tool_name, arguments, NULL, NULL, context, callback);
}

int tool_executor_submit_with_user_data(ToolExecutor* executor, const char* tool_name,
                                        const char* arguments, void* tool_user_data,
                                        ToolUserDataDestroyFunc tool_user_data_destroy,
                                        void* context,
                                        void (*callback)(void* context, const char* tool_name, const char* result, Error err)) {
    if (!executor || !tool_name) return -1;

    ToolExecutionRequest* request = calloc(1, sizeof(ToolExecutionRequest));
    if (!request) return -1;

    request->tool_name = strdup(tool_name);
    request->arguments = strdup(arguments ? arguments : "{}");
    if (!request->tool_name || !request->arguments) {
        free(request->tool_name);
        free(request->arguments);
        free(request);
        return -1;
    }
    request->tool_user_data_override = tool_user_data;
    request->tool_user_data_destroy = tool_user_data_destroy;
    request->context = context;
    request->callback = callback;
    request->next = NULL;

    // Enqueue request
    pthread_mutex_lock(&executor->queue_mutex);
    if (executor->request_queue_tail) {
        executor->request_queue_tail->next = request;
    } else {
        executor->request_queue = request;
    }
    executor->request_queue_tail = request;
    pthread_cond_signal(&executor->queue_cond);
    pthread_mutex_unlock(&executor->queue_mutex);

    return 0;
}

// Synchronous execution with timeout helper
typedef struct {
    String* result;
    Error* err;
    bool done;
    bool timed_out;
    int refs;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} SyncExecutionContext;

static void sync_context_release(SyncExecutionContext* ctx) {
    if (!ctx) return;
    bool should_free = false;
    pthread_mutex_lock(&ctx->mutex);
    ctx->refs--;
    if (ctx->refs == 0) {
        should_free = true;
    }
    pthread_mutex_unlock(&ctx->mutex);
    if (should_free) {
        pthread_mutex_destroy(&ctx->mutex);
        pthread_cond_destroy(&ctx->cond);
        free(ctx);
    }
}

static void sync_callback(void* context, const char* tool_name, const char* result, Error err) {
    (void)tool_name;  // May be used in future extensions
    SyncExecutionContext* ctx = (SyncExecutionContext*)context;
    pthread_mutex_lock(&ctx->mutex);
    if (!ctx->timed_out && result && ctx->result) {
        string_append(ctx->result, result);
    }
    if (!ctx->timed_out && ctx->err) {
        *ctx->err = err;
    }
    if (!ctx->timed_out) {
        ctx->done = true;
        pthread_cond_signal(&ctx->cond);
    }
    pthread_mutex_unlock(&ctx->mutex);
    sync_context_release(ctx);
}

// Forward declarations
static void async_callback_wrapper(void* context, const char* tool_name, const char* result, Error err);

typedef struct {
    ToolAsyncCallback original_callback;
    void* original_user_data;
} AsyncCallbackWrapperCtx;

static void async_callback_wrapper(void* context, const char* tool_name, const char* result, Error err) {
    (void)tool_name;
    AsyncCallbackWrapperCtx* ctx = (AsyncCallbackWrapperCtx*)context;
    if (ctx) {
        if (ctx->original_callback) {
            ctx->original_callback(err, result, ctx->original_user_data);
        }
        free(ctx);
    }
}

void tool_executor_submit_async(ToolExecutor* executor, const char* tool_name, const char* args_json, ToolAsyncCallback callback, void* user_data) {
    tool_executor_submit_async_with_user_data(executor, tool_name, args_json, NULL, NULL, callback, user_data);
}

void tool_executor_submit_async_with_user_data(ToolExecutor* executor, const char* tool_name, const char* args_json,
                                               void* tool_user_data, ToolUserDataDestroyFunc tool_user_data_destroy,
                                               ToolAsyncCallback callback, void* user_data) {
    AsyncCallbackWrapperCtx* ctx = malloc(sizeof(AsyncCallbackWrapperCtx));
    if (!ctx) {
        if (tool_user_data && tool_user_data_destroy) tool_user_data_destroy(tool_user_data);
        if (callback) callback(error_new(ERR_MEMORY, "OOM allocating async context"), NULL, user_data);
        return;
    }
    ctx->original_callback = callback;
    ctx->original_user_data = user_data;
    int ret = tool_executor_submit_with_user_data(executor, tool_name, args_json, tool_user_data, tool_user_data_destroy, ctx, async_callback_wrapper);
    if (ret != 0) {
        free(ctx);
        if (tool_user_data && tool_user_data_destroy) tool_user_data_destroy(tool_user_data);
        if (callback) callback(error_new(ERR_TOOL, "Failed to submit tool execution"), NULL, user_data);
    }
}

Error tool_executor_execute_sync(ToolExecutor* executor, const char* tool_name,
                                  const char* arguments, String* result, int timeout_ms) {
    if (!executor || !tool_name || !result) {
        return error_new(ERR_INVALID_PARAM, "Invalid parameters");
    }

    SyncExecutionContext* ctx = calloc(1, sizeof(SyncExecutionContext));
    if (!ctx) {
        return error_new(ERR_MEMORY, "Failed to allocate sync execution context");
    }

    Error sync_err = error_new(ERR_NONE, "");
    ctx->result = result;
    ctx->err = &sync_err;
    ctx->done = false;
    ctx->timed_out = false;
    ctx->refs = 2;
    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_cond_init(&ctx->cond, NULL);

    // Submit request
    int ret = tool_executor_submit(executor, tool_name, arguments, ctx, sync_callback);
    if (ret != 0) {
        sync_context_release(ctx);
        sync_context_release(ctx);
        return error_new(ERR_TOOL, "Failed to submit tool execution");
    }

    // Wait for completion with timeout
    pthread_mutex_lock(&ctx->mutex);

    if (timeout_ms > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }

        while (!ctx->done) {
            int rc = pthread_cond_timedwait(&ctx->cond, &ctx->mutex, &ts);
            if (rc == ETIMEDOUT) {
                ctx->timed_out = true;
                pthread_mutex_unlock(&ctx->mutex);
                sync_context_release(ctx);
                return error_new(ERR_TIMEOUT, "Tool execution timed out");
            }
        }
    } else {
        while (!ctx->done) {
            pthread_cond_wait(&ctx->cond, &ctx->mutex);
        }
    }

    pthread_mutex_unlock(&ctx->mutex);
    sync_context_release(ctx);

    return sync_err;
}
