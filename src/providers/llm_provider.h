#ifndef LLM_PROVIDER_H
#define LLM_PROVIDER_H

#include "../include/common.h"
#include "../include/message.h"
#include "../tools/tool.h"
#include "../include/config.h"
#include "../session/session.h"

typedef enum {
    FINISH_REASON_NONE = 0,
    FINISH_REASON_STOP,
    FINISH_REASON_LENGTH,
    FINISH_REASON_TOOL_CALLS,
    FINISH_REASON_CONTENT_FILTER,
    FINISH_REASON_ERROR
} FinishReason;

typedef struct {
    String content;
    ToolCall* tool_calls;
    size_t tool_calls_count;
    FinishReason finish_reason;
    int usage_tokens;
} LLMResponse;

typedef void (*StreamCallback)(const char* chunk, size_t len, bool is_done, void* user_data);

typedef struct {
    bool stream;
    StreamCallback on_chunk;
    void* user_data;
} LLMStreamOptions;

typedef void (*LLMAsyncCallback)(Error err, const char* response, ToolCall* tool_calls, size_t tool_calls_count, void* user_data);

typedef struct LLMAsyncManager LLMAsyncManager;

LLMAsyncManager* llm_async_manager_new();
void llm_async_manager_free(LLMAsyncManager* manager);
void llm_async_manager_start(LLMAsyncManager* manager);
void llm_async_manager_stop(LLMAsyncManager* manager);

void llm_provider_async_init(void);
void llm_provider_async_shutdown(void);
void llm_provider_call_async(const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, LLMAsyncCallback callback, void* user_data);

Error llm_provider_call(const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, String* response, ToolCall** tool_calls, size_t* tool_calls_count);

Error llm_provider_call_extended(const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, LLMResponse* llm_response);

Error llm_provider_call_streaming(const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, LLMResponse* llm_response, LLMStreamOptions* options);

void llm_provider_configure_mgr_dns(void* mgr, const Config* config);

#endif // LLM_PROVIDER_H
