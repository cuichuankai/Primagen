#ifndef LLM_PROVIDER_H
#define LLM_PROVIDER_H

#include "../include/common.h"
#include "../include/message.h"
#include "../tools/tool.h"
#include "../include/config.h"
#include "../session/session.h"

// Finish reasons from LLM
typedef enum {
    FINISH_REASON_NONE = 0,
    FINISH_REASON_STOP,
    FINISH_REASON_LENGTH,
    FINISH_REASON_TOOL_CALLS,
    FINISH_REASON_CONTENT_FILTER,
    FINISH_REASON_ERROR
} FinishReason;

// LLM response structure with finish reason
typedef struct {
    String content;
    ToolCall* tool_calls;
    size_t tool_calls_count;
    FinishReason finish_reason;
    int usage_tokens;
} LLMResponse;

// Streaming callback function type
// Called with each chunk of content as it arrives
// Parameters: chunk content, chunk length, is_done, user_data
typedef void (*StreamCallback)(const char* chunk, size_t len, bool is_done, void* user_data);

// Extended config for streaming requests
typedef struct {
    bool stream;
    StreamCallback on_chunk;
    void* user_data;
} LLMStreamOptions;

struct mg_mgr;

void llm_provider_configure_mgr_dns(struct mg_mgr* mgr, const Config* config);

// Async interface callback
typedef void (*LLMAsyncCallback)(Error err, const char* response, ToolCall* tool_calls, size_t tool_calls_count, void* user_data);

// Async request payload structure
typedef struct {
    char* system_prompt;
    Session* session;
    ToolRegistry* tools;
    Config* config;
    LLMAsyncCallback callback;
    void* user_data;
} LLMAsyncRequest;

// Async network lifecycle
void llm_provider_async_init(void);
void llm_provider_async_shutdown(void);
void llm_provider_call_async(const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, LLMAsyncCallback callback, void* user_data);

// LLM provider interface
Error llm_provider_call(const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, String* response, ToolCall** tool_calls, size_t* tool_calls_count);

// Extended interface with finish reason
Error llm_provider_call_extended(const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, LLMResponse* llm_response);

// Streaming interface
Error llm_provider_call_streaming(const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, LLMResponse* llm_response, LLMStreamOptions* options);

#endif // LLM_PROVIDER_H
