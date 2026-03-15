/**
 * Utility functions for Primagen
 * Provides common helpers used across modules
 */

#include "../include/common.h"
#include "../include/message.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * Strip <think>...</think> tags from text
 * Some models (like DeepSeek) embed thinking process in output
 */
char* strip_think_tags(const char* text) {
    if (!text || strlen(text) == 0) return NULL;

    const char* think_start = strstr(text, "<think>");
    const char* think_end = strstr(text, "</think>");

    if (!think_start || !think_end) {
        return strdup(text);
    }

    // Check if </think> comes after <think>
    if (think_end < think_start) {
        return strdup(text);
    }

    // Calculate lengths
    size_t prefix_len = think_start - text;
    const char* after_think = think_end + strlen("</think>");
    size_t suffix_len = strlen(after_think);

    // Allocate result
    char* result = malloc(prefix_len + suffix_len + 1);
    if (!result) return NULL;

    // Copy prefix (before <think>)
    if (prefix_len > 0) {
        memcpy(result, text, prefix_len);
    }

    // Copy suffix (after </think>)
    memcpy(result + prefix_len, after_think, suffix_len);
    result[prefix_len + suffix_len] = '\0';

    // Trim whitespace
    char* trimmed = result;
    while (isspace((unsigned char)*trimmed)) trimmed++;

    if (trimmed != result) {
        memmove(result, trimmed, strlen(trimmed) + 1);
    }

    // Trim trailing whitespace
    size_t len = strlen(result);
    while (len > 0 && isspace((unsigned char)result[len - 1])) {
        result[--len] = '\0';
    }

    return result;
}

/**
 * Format tool calls as hint string
 * e.g., web_search("query..."), read_file("config.json")
 */
char* format_tool_hint(ToolCall* tool_calls, size_t count) {
    if (!tool_calls || count == 0) return NULL;

    // Estimate buffer size
    size_t estimate = 0;
    for (size_t i = 0; i < count; i++) {
        estimate += tool_calls[i].name.len + 40; // name + args + formatting
    }

    char* result = malloc(estimate + 1);
    if (!result) return NULL;
    result[0] = '\0';

    for (size_t i = 0; i < count; i++) {
        if (i > 0) strcat(result, ", ");

        strcat(result, tool_calls[i].name.data);
        strcat(result, "(");

        // Try to extract first string argument value
        const char* args = tool_calls[i].arguments.data;
        const char* quote_start = strchr(args, '"');
        if (quote_start) {
            const char* quote_end = strchr(quote_start + 1, '"');
            if (quote_end) {
                size_t val_len = quote_end - quote_start - 1;
                if (val_len > 40) val_len = 40;
                strncat(result, quote_start + 1, val_len);
                if (val_len >= 40) strcat(result, "...");
            }
        }

        strcat(result, ")");
    }

    return result;
}

/**
 * Simple token estimation (rough approximation)
 * Assumes ~4 characters per token for English text
 */
size_t estimate_tokens(const char* text) {
    if (!text || strlen(text) == 0) return 0;
    return strlen(text) / 4;
}

/**
 * Estimate tokens in a message
 */
size_t estimate_message_tokens(const char* role, const char* content, int tool_calls_count) {
    size_t tokens = 0;

    // Role overhead
    if (role) {
        tokens += estimate_tokens(role) + 2; // role name + formatting
    }

    // Content
    if (content) {
        tokens += estimate_tokens(content);
    }

    // Tool calls overhead
    if (tool_calls_count > 0) {
        tokens += tool_calls_count * 5; // Approximate overhead per tool call
    }

    return tokens;
}

/**
 * Check if string is a placeholder value
 */
bool is_placeholder_value(const char* s) {
    if (!s || s[0] == '\0') return true;
    if (strcmp(s, "current") == 0 || strcmp(s, "chat") == 0 ||
        strcmp(s, "user") == 0 || strcmp(s, "assistant") == 0) return true;
    if (strncmp(s, "_user_", 6) == 0 || strncmp(s, "_assistant_", 11) == 0) return true;
    return false;
}

/**
 * Escape special XML characters
 */
char* escape_xml(const char* s) {
    if (!s) return strdup("");

    // Calculate needed length
    size_t len = 0;
    const char* p = s;
    while (*p) {
        if (*p == '&') len += 5;      // &amp;
        else if (*p == '<') len += 4; // &lt;
        else if (*p == '>') len += 4; // &gt;
        else if (*p == '"') len += 6; // &quot;
        else if (*p == '\'') len += 6; // &apos;
        else len++;
        p++;
    }

    char* out = malloc(len + 1);
    if (!out) return NULL;

    char* d = out;
    p = s;
    while (*p) {
        if (*p == '&') { strcpy(d, "&amp;"); d += 5; }
        else if (*p == '<') { strcpy(d, "&lt;"); d += 4; }
        else if (*p == '>') { strcpy(d, "&gt;"); d += 4; }
        else if (*p == '"') { strcpy(d, "&quot;"); d += 6; }
        else if (*p == '\'') { strcpy(d, "&apos;"); d += 6; }
        else { *d++ = *p; }
        p++;
    }
    *d = '\0';

    return out;
}

/**
 * Generate a unique ID (timestamp-based)
 */
char* generate_id(const char* prefix) {
    char id[64];
    time_t now = time(NULL);
    unsigned int rand_val = rand() % 0xFFFF;

    if (prefix) {
        snprintf(id, sizeof(id), "%s_%ld_%x", prefix, (long)now, rand_val);
    } else {
        snprintf(id, sizeof(id), "id_%ld_%x", (long)now, rand_val);
    }

    return strdup(id);
}

/**
 * Ensure directory exists (create if needed)
 */
int ensure_directory(const char* path) {
    char tmp[512];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);

    // Remove trailing slash
    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    // Find last slash to get parent directory
    char* last_slash = strrchr(tmp, '/');
    if (!last_slash) return 0; // No directory part

    *last_slash = '\0';

    // Create parent directories
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);

    return 0;
}
