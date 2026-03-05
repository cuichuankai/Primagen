#include "tool.h"
#include "../include/common.h"

ToolRegistry* tool_registry_new() {
    ToolRegistry* reg = malloc(sizeof(ToolRegistry));
    if (!reg) return NULL;
    reg->count = 0;
    reg->capacity = 8;
    reg->tools = malloc(reg->capacity * sizeof(Tool));
    return reg;
}

void tool_registry_free(ToolRegistry* reg) {
    if (!reg) return;
    for (size_t i = 0; i < reg->count; i++) {
        string_free(&reg->tools[i].def.name);
        string_free(&reg->tools[i].def.description);
        string_free(&reg->tools[i].def.parameters);
    }
    free(reg->tools);
    free(reg);
}

Error tool_registry_register(ToolRegistry* reg, const char* name, const char* desc, const char* params_schema, ToolExecuteFunc exec, void* user_data) {
    if (reg->count >= reg->capacity) {
        reg->capacity *= 2;
        reg->tools = realloc(reg->tools, reg->capacity * sizeof(Tool));
    }
    Tool* tool = &reg->tools[reg->count];
    tool->def.name = string_new(name);
    tool->def.description = string_new(desc);
    tool->def.parameters = string_new(params_schema);
    tool->execute = exec;
    tool->user_data = user_data;
    reg->count++;
    return error_new(ERR_NONE, "");
}

Tool* tool_registry_get(ToolRegistry* reg, const char* name) {
    String name_str = string_new(name);
    for (size_t i = 0; i < reg->count; i++) {
        if (string_equals(&reg->tools[i].def.name, &name_str)) {
            string_free(&name_str);
            return &reg->tools[i];
        }
    }
    string_free(&name_str);
    return NULL;
}

Error tool_registry_execute(ToolRegistry* reg, const char* name, const char* args_json, String* result) {
    Tool* tool = tool_registry_get(reg, name);
    if (!tool) {
        return error_new(ERR_TOOL, "Tool not found");
    }
    return tool->execute(tool->user_data, args_json, result);
}
