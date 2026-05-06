#include "tool.h"
#include "../include/common.h"

ToolRegistry* tool_registry_new() {
    ToolRegistry* reg = malloc(sizeof(ToolRegistry));
    if (!reg) return NULL;
    reg->count = 0;
    reg->capacity = 8;
    reg->tools = malloc(reg->capacity * sizeof(Tool));
    if (!reg->tools) {
        free(reg);
        return NULL;
    }
    return reg;
}

void tool_registry_free(ToolRegistry* reg) {
    if (!reg) return;
    void* freed_ptrs[64];
    size_t freed_count = 0;
    for (size_t i = 0; i < reg->count; i++) {
        string_free(&reg->tools[i].def.name);
        string_free(&reg->tools[i].def.description);
        string_free(&reg->tools[i].def.parameters);
        if (reg->tools[i].user_data && reg->tools[i].plugin_ref == NULL) {
            bool already_freed = false;
            for (size_t j = 0; j < freed_count; j++) {
                if (freed_ptrs[j] == reg->tools[i].user_data) {
                    already_freed = true;
                    break;
                }
            }
            if (!already_freed) {
                if (reg->tools[i].user_data_destroy) {
                    reg->tools[i].user_data_destroy(reg->tools[i].user_data);
                } else {
                    free(reg->tools[i].user_data);
                }
                if (freed_count < 64) freed_ptrs[freed_count++] = reg->tools[i].user_data;
            }
        }
    }
    free(reg->tools);
    free(reg);
}

Error tool_registry_register(ToolRegistry* reg, const char* name, const char* desc, const char* params_schema, ToolExecuteFunc exec, void* user_data) {
    return tool_registry_register_full(reg, name, desc, params_schema, exec, user_data, NULL, NULL);
}

Error tool_registry_register_full(ToolRegistry* reg, const char* name, const char* desc,
                                   const char* params_schema, ToolExecuteFunc exec,
                                   void* user_data, void* plugin_ref,
                                   ToolUserDataDestroyFunc user_data_destroy) {
    if (reg->count >= reg->capacity) {
        size_t new_cap = reg->capacity * 2;
        Tool* new_tools = realloc(reg->tools, new_cap * sizeof(Tool));
        if (!new_tools) return error_new(ERR_MEMORY, "Failed to expand tool registry");
        reg->tools = new_tools;
        reg->capacity = new_cap;
    }
    Tool* tool = &reg->tools[reg->count];
    tool->def.name = string_new(name);
    tool->def.description = string_new(desc);
    tool->def.parameters = string_new(params_schema);
    tool->execute = exec;
    tool->user_data = user_data;
    tool->plugin_ref = plugin_ref;
    tool->user_data_destroy = user_data_destroy;
    reg->count++;
    return error_new(ERR_NONE, "");
}

Error tool_registry_register_plugin_tool(ToolRegistry* reg, const char* name, const char* desc,
                                          const char* params_schema, ToolExecuteFunc exec,
                                          void* user_data, void* plugin_ref) {
    return tool_registry_register_full(reg, name, desc, params_schema, exec, user_data, plugin_ref, NULL);
}

Tool* tool_registry_get(ToolRegistry* reg, const char* name) {
    if (!reg || !name) return NULL;
    for (size_t i = 0; i < reg->count; i++) {
        if (reg->tools[i].def.name.data && strcmp(reg->tools[i].def.name.data, name) == 0) {
            return &reg->tools[i];
        }
    }
    const char* alias = strrchr(name, '.');
    if (!alias) alias = strrchr(name, ':');
    if (alias && *(alias + 1) != '\0') {
        const char* alias_name = alias + 1;
        for (size_t i = 0; i < reg->count; i++) {
            if (reg->tools[i].def.name.data && strcmp(reg->tools[i].def.name.data, alias_name) == 0) {
                return &reg->tools[i];
            }
        }
    }
    return NULL;
}

Error tool_registry_execute(ToolRegistry* reg, const char* name, const char* args_json, String* result) {
    Tool* tool = tool_registry_get(reg, name);
    if (!tool) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Tool not found: %s", name ? name : "(null)");
        return error_new(ERR_TOOL, buf);
    }
    return tool->execute(tool->user_data, args_json, result);
}
