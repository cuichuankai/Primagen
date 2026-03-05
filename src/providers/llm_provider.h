#ifndef LLM_PROVIDER_H
#define LLM_PROVIDER_H

#include "../include/common.h"
#include "../include/message.h"
#include "../tools/tool.h"
#include "../include/config.h"
#include "../session/session.h"

// LLM provider interface
Error llm_provider_call(const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, String* response, ToolCall** tool_calls, size_t* tool_calls_count);

#endif // LLM_PROVIDER_H
