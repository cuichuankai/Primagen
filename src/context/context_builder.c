#include "context_builder.h"
#include "../include/common.h"
#include "../include/skills.h"

ContextBuilder* context_builder_new(const char* workspace) {
    ContextBuilder* cb = malloc(sizeof(ContextBuilder));
    if (!cb) return NULL;
    cb->identity = string_new("");
    cb->bootstrap_files = string_array_new();
    cb->memory = NULL;
    cb->skills_loader = skills_loader_create(workspace);
    return cb;
}

void context_builder_free(ContextBuilder* cb) {
    if (!cb) return;
    string_free(&cb->identity);
    string_array_free(&cb->bootstrap_files);
    skills_loader_destroy(cb->skills_loader);
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
    if (cb->memory) {
        string_append(&prompt, cb->memory->memory_md.data);
        string_append(&prompt, "\n\n---\n\n");
    }

    // Always skills
    StringArray* always_skills = skills_loader_get_always_skills(cb->skills_loader);
    if (always_skills->count > 0) {
        char* always_content = skills_loader_load_skills_for_context(cb->skills_loader, always_skills);
        if (always_content) {
            string_append(&prompt, "# Active Skills\n\n");
            string_append(&prompt, always_content);
            string_append(&prompt, "\n\n---\n\n");
            free(always_content);
        }
    }
    string_array_free(always_skills);
    free(always_skills);

    // Skills summary
    char* skills_summary = skills_loader_build_skills_summary(cb->skills_loader);
    if (skills_summary) {
        string_append(&prompt, "# Skills\n\n");
        string_append(&prompt, skills_summary);
        string_append(&prompt, "\n\n---\n\n");
        free(skills_summary);
    }

    // Session history (last N messages)
    size_t start = session->messages.count > 10 ? session->messages.count - 10 : 0;
    for (size_t i = start; i < session->messages.count; i++) {
        Message* msg = *(Message**)dynamic_array_get(&session->messages, i);
        const char* role_str = msg->role == ROLE_USER ? "User" : msg->role == ROLE_ASSISTANT ? "Assistant" : "Tool";
        string_append(&prompt, role_str);
        string_append(&prompt, ": ");
        string_append(&prompt, msg->content.data);
        string_append(&prompt, "\n");
    }

    return prompt;
}