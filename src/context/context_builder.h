#ifndef CONTEXT_BUILDER_H
#define CONTEXT_BUILDER_H

#include "../include/common.h"
#include "../include/skills.h"
#include "../session/session.h"
#include "../memory/memory.h"
#include "../tools/tool.h"

#define DEFAULT_CONTEXT_WINDOW 128000
#define CONTEXT_RESERVE_TOKENS 8192

typedef struct {
    String identity;
    StringArray bootstrap_files;
    Memory* memory;
    SkillsLoader* skills_loader;
    char* workspace;

    char* cached_memory_content;
    char* cached_skills_content;
    char* cached_skills_summary;
    time_t memory_mtime;
    time_t skills_mtime;
    
    size_t context_window;
    size_t max_history_tokens;
} ContextBuilder;

ContextBuilder* context_builder_new(const char* workspace);
void context_builder_free(ContextBuilder* cb);
void context_builder_set_identity(ContextBuilder* cb, const char* identity);
void context_builder_add_bootstrap(ContextBuilder* cb, const char* file_content);
void context_builder_set_memory(ContextBuilder* cb, Memory* mem);
void context_builder_set_context_window(ContextBuilder* cb, size_t window_tokens);
String context_builder_build(ContextBuilder* cb, Session* session, ToolRegistry* tools);
String context_builder_build_with_channel(ContextBuilder* cb, Session* session, ToolRegistry* tools, const char* channel, const char* chat_id);

#endif