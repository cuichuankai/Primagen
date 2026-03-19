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
        // Free user_data for builtin tools (no plugin_ref means it was allocated by agent_loop)
        if (reg->tools[i].user_data && reg->tools[i].plugin_ref == NULL) {
            free(reg->tools[i].user_data);
        }
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
    tool->plugin_ref = NULL;  // NULL means builtin tool
    reg->count++;
    return error_new(ERR_NONE, "");
}

// Plugin-aware tool registration
Error tool_registry_register_plugin_tool(ToolRegistry* reg, const char* name, const char* desc,
                                          const char* params_schema, ToolExecuteFunc exec,
                                          void* user_data, void* plugin_ref) {
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
    tool->plugin_ref = plugin_ref;  // Track which plugin this tool comes from
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
    if (name) {
        const char* alias = strrchr(name, '.');
        if (!alias) alias = strrchr(name, ':');
        if (alias && *(alias + 1) != '\0') {
            String alias_str = string_new(alias + 1);
            for (size_t i = 0; i < reg->count; i++) {
                if (string_equals(&reg->tools[i].def.name, &alias_str)) {
                    string_free(&alias_str);
                    string_free(&name_str);
                    return &reg->tools[i];
                }
            }
            string_free(&alias_str);
        }
    }
    string_free(&name_str);
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
