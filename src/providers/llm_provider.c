#include "llm_provider.h"
#include "../include/common.h"

Error stub_llm_call(const char* prompt, String* response, ToolCall** tool_calls, size_t* tool_calls_count) {
    // Simulate LLM response
    *response = string_new("This is a simulated response from the LLM.");
    *tool_calls = NULL;
    *tool_calls_count = 0;
    return error_new(ERR_NONE, "");
}