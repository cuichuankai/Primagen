#include "context_builder.h"
#include "../include/common.h"

ContextBuilder* context_builder_new() {
    ContextBuilder* cb = malloc(sizeof(ContextBuilder));
    if (!cb) return NULL;
    cb->identity = string_new("");
    cb->bootstrap_files = string_array_new();
    cb->memory = NULL;
    cb->skills = string_array_new();
    return cb;
}

void context_builder_free(ContextBuilder* cb) {
    if (!cb) return;
    string_free(&cb->identity);
    string_array_free(&cb->bootstrap_files);
    string_array_free(&cb->skills);
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

void context_builder_add_skill(ContextBuilder* cb, const char* skill_content) {
    string_array_add(&cb->skills, skill_content);
}

String context_builder_build(ContextBuilder* cb, Session* session, ToolRegistry* tools) {
    String prompt = string_new("");
    
    // Identity
    if (cb->identity.len > 0) {
        prompt = string_copy(&cb->identity);
        prompt.data = realloc(prompt.data, prompt.len + 10);
        strcat(prompt.data, "\n\n---\n\n");
        prompt.len = strlen(prompt.data);
    }
    
    // Bootstrap files
    for (size_t i = 0; i < cb->bootstrap_files.count; i++) {
        String* file = &cb->bootstrap_files.items[i];
        prompt.data = realloc(prompt.data, prompt.len + file->len + 10);
        strcat(prompt.data, file->data);
        strcat(prompt.data, "\n\n---\n\n");
        prompt.len = strlen(prompt.data);
    }
    
    // Memory
    if (cb->memory) {
        prompt.data = realloc(prompt.data, prompt.len + cb->memory->memory_md.len + 10);
        strcat(prompt.data, cb->memory->memory_md.data);
        strcat(prompt.data, "\n\n---\n\n");
        prompt.len = strlen(prompt.data);
    }
    
    // Skills
    for (size_t i = 0; i < cb->skills.count; i++) {
        String* skill = &cb->skills.items[i];
        prompt.data = realloc(prompt.data, prompt.len + skill->len + 10);
        strcat(prompt.data, skill->data);
        strcat(prompt.data, "\n\n---\n\n");
        prompt.len = strlen(prompt.data);
    }
    
    // Session history (last N messages)
    size_t start = session->messages.count > 10 ? session->messages.count - 10 : 0;
    for (size_t i = start; i < session->messages.count; i++) {
        Message* msg = *(Message**)dynamic_array_get(&session->messages, i);
        const char* role_str = msg->role == ROLE_USER ? "User" : msg->role == ROLE_ASSISTANT ? "Assistant" : "Tool";
        prompt.data = realloc(prompt.data, prompt.len + strlen(role_str) + msg->content.len + 10);
        strcat(prompt.data, role_str);
        strcat(prompt.data, ": ");
        strcat(prompt.data, msg->content.data);
        strcat(prompt.data, "\n");
        prompt.len = strlen(prompt.data);
    }
    
    return prompt;
}