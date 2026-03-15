#include "context_builder.h"
#include "../include/common.h"
#include "../include/skills.h"
#include <time.h>

/* Build runtime context metadata */
static char* build_runtime_context(const char* channel, const char* chat_id) {
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);

    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M (%A)", tm_info);

    // Get timezone
    char tz_buf[32];
    strftime(tz_buf, sizeof(tz_buf), "%Z", tm_info);
    if (tz_buf[0] == '\0') {
        strcpy(tz_buf, "UTC");
    }

    // Calculate buffer size
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
    return cb;
}

void context_builder_free(ContextBuilder* cb) {
    if (!cb) return;
    string_free(&cb->identity);
    string_array_free(&cb->bootstrap_files);
    skills_loader_destroy(cb->skills_loader);
    free(cb->workspace);
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

    // Memory
    if (cb->memory && cb->workspace) {
        // Reload memory to ensure latest content
        memory_load(cb->memory, cb->workspace);

        string_append(&prompt, cb->memory->memory_md.data);
        string_append(&prompt, "\n\n---\n\n");
    } else {
        // Fallback if memory not set (should not happen in prod)
        string_append(&prompt, "# Long-term Memory\n\n(No memory loaded)\n\n---\n\n");
    }

    // Always skills
    if (cb->skills_loader) {
        StringArray* always_skills = skills_loader_get_always_skills(cb->skills_loader);
        if (always_skills && always_skills->count > 0) {
            char* always_content = skills_loader_load_skills_for_context(cb->skills_loader, always_skills);
            if (always_content) {
                string_append(&prompt, "# Active Skills\n\n");
                string_append(&prompt, always_content);
                string_append(&prompt, "\n\n---\n\n");
                free(always_content);
            }
            string_array_free(always_skills);
            free(always_skills);
        }
    }

    // Skills summary
    if (cb->skills_loader) {
        char* skills_summary = skills_loader_build_skills_summary(cb->skills_loader);
        if (skills_summary) {
            string_append(&prompt, "# Available Skills\n\n");
            string_append(&prompt, "Skills extend your capabilities. To use a skill:\n");
            string_append(&prompt, "1. Use `skill` tool with action='load' and name='skill-name' to read the SKILL.md\n");
            string_append(&prompt, "2. Follow the skill's instructions to complete the task (may involve using other tools like `exec`)\n");
            string_append(&prompt, "IMPORTANT: Skills are NOT directly callable as tools. Always use `skill` tool first to load the skill.\n\n");
            string_append(&prompt, skills_summary);
            string_append(&prompt, "\n\n---\n\n");
            free(skills_summary);
        }
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
