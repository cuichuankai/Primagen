/**
 * Utility functions header
 */

#ifndef UTILS_H
#define UTILS_H

#include "../include/common.h"
#include "../include/message.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * Strip <think>...</think> tags from text
 * Some models (like DeepSeek) embed thinking process in output
 */
char* strip_think_tags(const char* text);

/**
 * Format tool calls as hint string
 * e.g., web_search("query..."), read_file("config.json")
 */
char* format_tool_hint(ToolCall* tool_calls, size_t count);

/**
 * Simple token estimation (rough approximation)
 * Assumes ~4 characters per token for English text
 */
size_t estimate_tokens(const char* text);

/**
 * Estimate tokens in a message
 */
size_t estimate_message_tokens(const char* role, const char* content, int tool_calls_count);

/**
 * Check if string is a placeholder value
 */
bool is_placeholder_value(const char* s);

/**
 * Escape special XML characters
 */
char* escape_xml(const char* s);

/**
 * Generate a unique ID (timestamp-based)
 */
char* generate_id(const char* prefix);

/**
 * Ensure directory exists (create if needed)
 */
int ensure_directory(const char* path);

const char* str_trim_left(const char* s);

char* str_trim_copy(const char* s);

#endif // UTILS_H
