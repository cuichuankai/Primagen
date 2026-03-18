#ifndef CONTEXT_BUILDER_H
#define CONTEXT_BUILDER_H

#include "../include/common.h"
#include "../include/skills.h"
#include "../session/session.h"
#include "../memory/memory.h"
#include "../tools/tool.h"

typedef struct {
    String identity;
    StringArray bootstrap_files;
    Memory* memory;
    SkillsLoader* skills_loader;
    char* workspace;

    // Caching for LLM efficiency
    char* cached_memory_content;      // Cached memory content
    char* cached_skills_content;      // Cached always-on skills content
    char* cached_skills_summary;      // Cached skills summary
    time_t memory_mtime;              // Last modification time of memory files
    time_t skills_mtime;              // Last modification time of skills files
} ContextBuilder;

// Functions
ContextBuilder* context_builder_new(const char* workspace);
void context_builder_free(ContextBuilder* cb);
void context_builder_set_identity(ContextBuilder* cb, const char* identity);
void context_builder_add_bootstrap(ContextBuilder* cb, const char* file_content);
void context_builder_set_memory(ContextBuilder* cb, Memory* mem);
String context_builder_build(ContextBuilder* cb, Session* session, ToolRegistry* tools);
String context_builder_build_with_channel(ContextBuilder* cb, Session* session, ToolRegistry* tools, const char* channel, const char* chat_id);

#endif // CONTEXT_BUILDER_H