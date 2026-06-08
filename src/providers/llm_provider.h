#ifndef LLM_PROVIDER_H
#define LLM_PROVIDER_H

#include "../include/common.h"
#include "../include/message.h"
#include "../tools/tool.h"
#include "../include/config.h"
#include "../session/session.h"
#include <pthread.h>

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

// =============================================================================
// LLM Provider Plugin Interface
// =============================================================================

typedef struct LLMProvider LLMProvider;

typedef Error (*LLMProviderInitFunc)(LLMProvider* provider, Config* config);
typedef void (*LLMProviderShutdownFunc)(LLMProvider* provider);
typedef Error (*LLMProviderCallFunc)(LLMProvider* provider, const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, String* response, ToolCall** tool_calls, size_t* tool_calls_count);
typedef Error (*LLMProviderCallExtendedFunc)(LLMProvider* provider, const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, LLMResponse* llm_response);
typedef void (*LLMProviderCallAsyncFunc)(LLMProvider* provider, const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, LLMAsyncCallback callback, void* user_data);
typedef Error (*LLMProviderCallStreamingFunc)(LLMProvider* provider, const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, LLMResponse* llm_response, LLMStreamOptions* options);

typedef struct LLMProviderInterface {
    const char* name;
    LLMProviderInitFunc init;
    LLMProviderShutdownFunc shutdown;
    LLMProviderCallFunc call;
    LLMProviderCallExtendedFunc call_extended;
    LLMProviderCallAsyncFunc call_async;
    LLMProviderCallStreamingFunc call_streaming;
} LLMProviderInterface;

struct LLMProvider {
    const LLMProviderInterface* iface;
    char* name;
    void* state;
    void* plugin_ref;
    bool initialized;
};

// =============================================================================
// LLM Provider Registry
// =============================================================================

typedef struct LLMProviderRegistry {
    LLMProvider** providers;
    size_t count;
    size_t capacity;
    LLMProvider* active;
    pthread_mutex_t lock;
} LLMProviderRegistry;

LLMProviderRegistry* llm_provider_registry_new(void);
void llm_provider_registry_free(LLMProviderRegistry* registry);
int llm_provider_registry_register(LLMProviderRegistry* registry, LLMProvider* provider);
size_t llm_provider_registry_unregister_by_plugin(LLMProviderRegistry* registry, void* plugin_ref);
LLMProvider* llm_provider_registry_find(LLMProviderRegistry* registry, const char* id);
int llm_provider_registry_set_active(LLMProviderRegistry* registry, const char* id);
LLMProvider* llm_provider_registry_get_active(LLMProviderRegistry* registry);

LLMProviderRegistry* llm_provider_get_registry(void);

LLMProvider* llm_provider_new(const LLMProviderInterface* iface, const char* name);
void llm_provider_free(LLMProvider* provider);

#endif // LLM_PROVIDER_H
