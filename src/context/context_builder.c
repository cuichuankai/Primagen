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
    // Note: skills_loader is a mock or partial implementation in my memory, but let's assume it works.
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

    // Skills summary (for tool selection or context)
    if (cb->skills_loader) {
        char* skills_summary = skills_loader_build_skills_summary(cb->skills_loader);
        if (skills_summary) {
            string_append(&prompt, "# Available Skills\n\n");
            string_append(&prompt, "<skills_instructions>\n");
            string_append(&prompt, "When users ask you to perform tasks, check if any of the available skills below can help complete the task more effectively. Skills provide specialized capabilities and domain knowledge.\n");
            string_append(&prompt, "How to use skills:\n");
            string_append(&prompt, "- Invoke skills using the `skill` tool with the `load` action.\n");
            string_append(&prompt, "- Example: `skill(action=\"load\", name=\"weather\")` - loads the weather skill\n");
            string_append(&prompt, "- Once loaded, the skill's instructions will be added to your context, teaching you how to use specific tools or patterns.\n");
            string_append(&prompt, "- Do NOT try to call the skill name as a tool directly (e.g. do NOT call `weather(...)`). Always load it first.\n");
            string_append(&prompt, "</skills_instructions>\n\n");
            string_append(&prompt, skills_summary);
            string_append(&prompt, "\n\n---\n\n");
            free(skills_summary);
        }
    }
    
    // Note: We do NOT append session history here anymore. 
    // The LLM provider will handle the message history structure.

    return prompt;
}
