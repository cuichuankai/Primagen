#ifndef CONTEXT_BUILDER_H
#define CONTEXT_BUILDER_H

#include "../include/common.h"
#include "../session/session.h"
#include "../memory/memory.h"
#include "../tools/tool.h"

typedef struct {
    String identity;
    StringArray bootstrap_files;
    Memory* memory;
    StringArray skills;
} ContextBuilder;

// Functions
ContextBuilder* context_builder_new();
void context_builder_free(ContextBuilder* cb);
void context_builder_set_identity(ContextBuilder* cb, const char* identity);
void context_builder_add_bootstrap(ContextBuilder* cb, const char* file_content);
void context_builder_set_memory(ContextBuilder* cb, Memory* mem);
void context_builder_add_skill(ContextBuilder* cb, const char* skill_content);
String context_builder_build(ContextBuilder* cb, Session* session, ToolRegistry* tools);

#endif // CONTEXT_BUILDER_H