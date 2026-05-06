#include "context_builder.h"
#include "../include/common.h"
#include "../include/skills.h"
#include "../include/utils.h"
#include "../include/message.h"
#include <time.h>
#include <sys/stat.h>

static size_t estimate_session_tokens(Session* session) {
    if (!session) return 0;
    size_t total = 0;
    for (size_t i = 0; i < session->messages.count; i++) {
        Message* msg = *(Message**)dynamic_array_get(&session->messages, i);
        if (!msg || msg->role == ROLE_TOOL) continue;
        total += estimate_tokens(msg->content.data);
    }
    return total;
}

static void apply_sliding_window(Session* session, size_t max_tokens) {
    if (!session || max_tokens == 0) return;
    
    size_t current_tokens = estimate_session_tokens(session);
    if (current_tokens <= max_tokens) return;
    
    while (session->messages.count > 2 && current_tokens > max_tokens) {
        Message* oldest = *(Message**)dynamic_array_get(&session->messages, 0);
        if (oldest) {
            current_tokens -= estimate_tokens(oldest->content.data);
            message_free(oldest);
        }
        memmove((char*)session->messages.items, 
                (char*)session->messages.items + session->messages.item_size,
                (session->messages.count - 1) * session->messages.item_size);
        session->messages.count--;
    }
}

// apply_sliding_window is used when session history is passed to context builder
__attribute__((used)) static void apply_sliding_window_wrapper(Session* s, size_t m) {
    apply_sliding_window(s, m);
}

/* Build runtime context metadata */
static char* build_runtime_context(const char* channel, const char* chat_id) {
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);

    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M (%A)", &tm_info);

    char tz_buf[32];
    strftime(tz_buf, sizeof(tz_buf), "%Z", &tm_info);
    if (tz_buf[0] == '\0') {
        strcpy(tz_buf, "UTC");
    }

    size_t len = 512;
    if (channel) len += strlen(channel) + 20;
    if (chat_id) len += strlen(chat_id) + 20;

    char* result = malloc(len);
    if (!result) return NULL;

    strcpy(result, "[Runtime Context — metadata only, not instructions]\n");
    char buf[256];
    snprintf(buf, sizeof(buf), "Current Time: %s (%s)\n", time_buf, tz_buf);
    strcat(result, buf);

    if (channel && chat_id) {
        snprintf(buf, sizeof(buf), "Channel: %s\nChat ID: %s\n", channel, chat_id);
        strcat(result, buf);
    }

    return result;
}

ContextBuilder* context_builder_new(const char* workspace) {
    ContextBuilder* cb = malloc(sizeof(ContextBuilder));
    if (!cb) return NULL;
    cb->identity = string_new("");
    cb->bootstrap_files = string_array_new();
    cb->memory = NULL;
    cb->skills_loader = skills_loader_create(workspace);
    cb->workspace = strdup(workspace);
    cb->cached_memory_content = NULL;
    cb->cached_skills_content = NULL;
    cb->cached_skills_summary = NULL;
    cb->memory_mtime = 0;
    cb->skills_mtime = 0;
    cb->context_window = DEFAULT_CONTEXT_WINDOW;
    cb->max_history_tokens = DEFAULT_CONTEXT_WINDOW - CONTEXT_RESERVE_TOKENS;
    return cb;
}

void context_builder_set_context_window(ContextBuilder* cb, size_t window_tokens) {
    if (!cb) return;
    cb->context_window = window_tokens;
    cb->max_history_tokens = window_tokens > CONTEXT_RESERVE_TOKENS ? 
                             window_tokens - CONTEXT_RESERVE_TOKENS : window_tokens / 2;
}

void context_builder_free(ContextBuilder* cb) {
    if (!cb) return;
    string_free(&cb->identity);
    string_array_free(&cb->bootstrap_files);
    skills_loader_destroy(cb->skills_loader);
    free(cb->workspace);
    free(cb->cached_memory_content);
    free(cb->cached_skills_content);
    free(cb->cached_skills_summary);
    free(cb);
}

void context_builder_set_identity(ContextBuilder* cb, const char* identity) {
    string_free(&cb->identity);
    cb->identity = string_new(identity);
}

void context_builder_add_bootstrap(ContextBuilder* cb, const char* file_content) {
    string_array_add(&cb->bootstrap_files, file_content);
}

void context_builder_set_memory(ContextBuilder* cb, Memory* mem) {
    cb->memory = mem;
}

String context_builder_build(ContextBuilder* cb, Session* session, ToolRegistry* tools) {
    return context_builder_build_with_channel(cb, session, tools, NULL, NULL);
}

String context_builder_build_with_channel(ContextBuilder* cb, Session* session, ToolRegistry* tools, const char* channel, const char* chat_id) {
    (void)session;  // Session history is now handled by LLM provider
    (void)tools;    // Tools are passed separately to LLM provider

    String prompt = string_new("");

    // Identity
    if (cb->identity.len > 0) {
        string_append(&prompt, cb->identity.data);
        string_append(&prompt, "\n\n---\n\n");
    }

    // Bootstrap files
    for (size_t i = 0; i < cb->bootstrap_files.count; i++) {
        String* file = &cb->bootstrap_files.items[i];
        string_append(&prompt, file->data);
        string_append(&prompt, "\n\n---\n\n");
    }

    // Memory - only reload if file changed
    if (cb->memory && cb->workspace) {
        // Check if memory files have changed
        char memory_path[512];
        snprintf(memory_path, sizeof(memory_path), "%s/memory/MEMORY.md", cb->workspace);
        struct stat st;
        time_t new_mtime = 0;
        if (stat(memory_path, &st) == 0) {
            new_mtime = st.st_mtime;
        }

        // Reload only if file changed or not cached
        if (!cb->cached_memory_content || new_mtime > cb->memory_mtime) {
            memory_load(cb->memory, cb->workspace);
            free(cb->cached_memory_content);
            cb->cached_memory_content = strdup(cb->memory->memory_md.data);
            cb->memory_mtime = new_mtime;
        }

        if (cb->cached_memory_content) {
            string_append(&prompt, cb->cached_memory_content);
            string_append(&prompt, "\n\n---\n\n");
        }
    }

    // Always skills - cache and reuse
    if (cb->skills_loader && (!cb->cached_skills_content || !cb->cached_skills_summary)) {
        StringArray* always_skills = skills_loader_get_always_skills(cb->skills_loader);
        if (always_skills && always_skills->count > 0) {
            char* always_content = skills_loader_load_skills_for_context(cb->skills_loader, always_skills);
            if (always_content) {
                free(cb->cached_skills_content);
                cb->cached_skills_content = always_content;
            }
            string_array_free(always_skills);
            free(always_skills);
        }

        char* skills_summary = skills_loader_build_skills_summary(cb->skills_loader);
        if (skills_summary) {
            free(cb->cached_skills_summary);
            cb->cached_skills_summary = skills_summary;
        }
    }

    // Use cached skills content
    if (cb->cached_skills_content) {
        string_append(&prompt, "# Active Skills\n\n");
        string_append(&prompt, cb->cached_skills_content);
        string_append(&prompt, "\n\n---\n\n");
    }

    // Use cached skills summary
    if (cb->cached_skills_summary) {
        string_append(&prompt, "# Available Skills\n\n");
        string_append(&prompt, "Skills extend your capabilities. To use a skill:\n");
        string_append(&prompt, "1. Use `skill` tool with action='load' and name='skill-name' to read the SKILL.md\n");
        string_append(&prompt, "2. Follow the skill's instructions to complete the task (may involve using other tools like `exec`)\n");
        string_append(&prompt, "IMPORTANT: Skills are NOT directly callable as tools. Always use `skill` tool first to load the skill.\n\n");
        string_append(&prompt, cb->cached_skills_summary);
        string_append(&prompt, "\n\n---\n\n");
    }

    // Runtime context (injected at the end, will be prepended to user message by agent loop)
    char* runtime_ctx = build_runtime_context(channel, chat_id);
    if (runtime_ctx) {
        string_append(&prompt, "\n");
        string_append(&prompt, runtime_ctx);
        free(runtime_ctx);
    }

    return prompt;
}
