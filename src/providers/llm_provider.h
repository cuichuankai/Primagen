#ifndef LLM_PROVIDER_H
#define LLM_PROVIDER_H

#include "../include/common.h"
#include "../include/message.h"

// Stub LLM provider that simulates responses
Error stub_llm_call(const char* prompt, String* response, ToolCall** tool_calls, size_t* tool_calls_count);

#endif // LLM_PROVIDER_H