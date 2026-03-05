#ifndef TOOL_H
#define TOOL_H

#include "../include/common.h"

typedef struct {
    String name;
    String description;
    String parameters; // JSON schema
} ToolDefinition;

typedef Error (*ToolExecuteFunc)(void* user_data, const char* args_json, String* result);

typedef struct {
    ToolDefinition def;
    ToolExecuteFunc execute;
    void* user_data;
} Tool;

typedef struct {
    Tool* tools;
    size_t count;
    size_t capacity;
} ToolRegistry;

// Functions
ToolRegistry* tool_registry_new();
void tool_registry_free(ToolRegistry* reg);
Error tool_registry_register(ToolRegistry* reg, const char* name, const char* desc, const char* params_schema, ToolExecuteFunc exec, void* user_data);
Tool* tool_registry_get(ToolRegistry* reg, const char* name);
Error tool_registry_execute(ToolRegistry* reg, const char* name, const char* args_json, String* result);

#endif // TOOL_H
