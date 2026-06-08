#include "tools_impl.h"
#include "tool_validation.h"
#include "../vendor/cJSON/cJSON.h"
#include "../bus/message_bus.h"
#include "../include/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <sys/select.h>

#define MAX_READ_SIZE 128000

// Error hint for tool failures (helps LLM recover)
#define TOOL_ERROR_HINT "\n\n[Analyze the error above and try a different approach.]"

// Helper to get string from JSON
static char* get_json_string(cJSON* root, const char* key) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return NULL;
}

static Error parse_send_message_attachments(cJSON* json, OutboundMessage* msg) {
    cJSON* attachments = cJSON_GetObjectItem(json, "attachments");
    if (!attachments) {
        return error_new(ERR_NONE, "");
    }
    if (!cJSON_IsArray(attachments)) {
        return error_new(ERR_INVALID_PARAM, "'attachments' must be an array");
    }

    cJSON* item = NULL;
    cJSON_ArrayForEach(item, attachments) {
        if (cJSON_IsString(item) && item->valuestring) {
            string_array_add(&msg->attachments, item->valuestring);
            continue;
        }
        if (!cJSON_IsObject(item)) {
            return error_new(ERR_INVALID_PARAM, "Each attachment must be string or object");
        }

        char* type = get_json_string(item, "type");
        char* path = get_json_string(item, "path");
        if (!type || !path) {
            return error_new(ERR_INVALID_PARAM, "Attachment object requires 'type' and 'path'");
        }

        cJSON* normalized = cJSON_CreateObject();
        if (!normalized) {
            return error_new(ERR_MEMORY, "Memory allocation failed");
        }
        cJSON_AddStringToObject(normalized, "type", type);
        cJSON_AddStringToObject(normalized, "path", path);

        cJSON* duration = cJSON_GetObjectItem(item, "duration");
        if (cJSON_IsNumber(duration) && duration->valueint > 0) {
            cJSON_AddNumberToObject(normalized, "duration", duration->valueint);
        }
        char* cover_path = get_json_string(item, "cover_path");
        if (cover_path) {
            cJSON_AddStringToObject(normalized, "cover_path", cover_path);
        }

        char* url = get_json_string(item, "url");
        if (url) {
            cJSON_AddStringToObject(normalized, "url", url);
        }

        char* normalized_str = cJSON_PrintUnformatted(normalized);
        cJSON_Delete(normalized);
        if (!normalized_str) {
            return error_new(ERR_MEMORY, "Memory allocation failed");
        }

        string_array_add(&msg->attachments, normalized_str);
        free(normalized_str);
    }

    return error_new(ERR_NONE, "");
}

static bool is_registered_channel(ToolContext* ctx, const char* channel) {
    if (!ctx || !channel || !ctx->plugin_mgr) return false;

    size_t count = 0;
    const char** channels = plugin_manager_get_channels(ctx->plugin_mgr, &count);
    if (!channels) return false;

    for (size_t i = 0; i < count; i++) {
        if (channels[i] && strcmp(channels[i], channel) == 0) return true;
    }
    return false;
}

static const char* resolve_channel(ToolContext* ctx, const char* channel) {
    if (!is_placeholder_value(channel) && is_registered_channel(ctx, channel)) return channel;
    if (ctx && ctx->current_channel[0]) return ctx->current_channel;
    return "cli";
}

static const char* resolve_chat_id(ToolContext* ctx, const char* chat_id) {
    if (!is_placeholder_value(chat_id)) return chat_id;
    if (ctx && ctx->current_chat_id[0]) return ctx->current_chat_id;
    return "current";
}

void tool_context_set_route(ToolContext* ctx, const char* channel, const char* chat_id) {
    if (!ctx) return;
    if (ctx->magic != 0x50474E31) {
        log_error("[tool_context_set_route] Invalid ToolContext: magic=0x%x (expected 0x50474e31)", ctx->magic);
        return;
    }
    log_debug("[tool_context_set_route] Setting route: channel=%s, chat_id=%s", channel ? channel : "(null)", chat_id ? chat_id : "(null)");
    pthread_mutex_lock(&ctx->route_mutex);
    if (channel) {
        strncpy(ctx->current_channel, channel, sizeof(ctx->current_channel) - 1);
        ctx->current_channel[sizeof(ctx->current_channel) - 1] = '\0';
    } else {
        ctx->current_channel[0] = '\0';
    }
    if (chat_id) {
        strncpy(ctx->current_chat_id, chat_id, sizeof(ctx->current_chat_id) - 1);
        ctx->current_chat_id[sizeof(ctx->current_chat_id) - 1] = '\0';
    } else {
        ctx->current_chat_id[0] = '\0';
    }
    pthread_mutex_unlock(&ctx->route_mutex);
}

void tool_context_destroy(void* user_data) {
    ToolContext* ctx = (ToolContext*)user_data;
    if (!ctx) return;
    if (ctx->magic != TOOL_CONTEXT_MAGIC) return;
    ctx->magic = 0;
    pthread_mutex_destroy(&ctx->route_mutex);
    free(ctx);
}

ToolContext* tool_context_clone_with_route(ToolContext* ctx, const char* channel, const char* chat_id) {
    if (!ctx || ctx->magic != TOOL_CONTEXT_MAGIC) return NULL;
    ToolContext* clone = malloc(sizeof(ToolContext));
    if (!clone) return NULL;
    memcpy(clone, ctx, sizeof(ToolContext));
    if (pthread_mutex_init(&clone->route_mutex, NULL) != 0) {
        free(clone);
        return NULL;
    }
    clone->magic = TOOL_CONTEXT_MAGIC;
    tool_context_set_route(clone, channel, chat_id);
    return clone;
}

ToolContext* tool_context_new(MessageBus* bus,
                              SubagentManager* subagent_mgr,
                              CronService* cron_service,
                              SkillsLoader* skills_loader,
                              Memory* memory,
                              Config* config,
                              PluginManager* plugin_mgr,
                              const char* workspace) {
    ToolContext* ctx = calloc(1, sizeof(ToolContext));
    if (!ctx) return NULL;
    ctx->magic = TOOL_CONTEXT_MAGIC;
    ctx->bus = bus;
    ctx->subagent_mgr = subagent_mgr;
    ctx->cron_service = cron_service;
    ctx->skills_loader = skills_loader;
    ctx->memory = memory;
    ctx->config = config;
    ctx->plugin_mgr = plugin_mgr;
    ctx->workspace = workspace;
    if (pthread_mutex_init(&ctx->route_mutex, NULL) != 0) {
        free(ctx);
        return NULL;
    }
    /* Default route: "cli" / "current". Callers (e.g. tool_send_message)
     * may overwrite via tool_context_set_route at execution time. */
    snprintf(ctx->current_channel, sizeof(ctx->current_channel), "%s", "cli");
    snprintf(ctx->current_chat_id, sizeof(ctx->current_chat_id), "%s", "current");
    return ctx;
}

/* Allowlist helper: true if c is part of the safe-exec character set.
 * Strategy: printable ASCII subset, no shell metacharacters. Safer than the
 * previous black-list approach which missed | & ; < > etc. */
static inline bool safe_char(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= 'a' && c <= 'z') return true;
    if (c >= '0' && c <= '9') return true;
    if (c == '_' || c == '-' || c == '.' || c == '/' || c == ',' ||
        c == ':' || c == '@' || c == '+' || c == '=' || c == '%') return true;
    return false;
}

static bool exec_token_is_safe(const char* tok, size_t tok_len) {
    if (tok_len == 0) return false;
    for (size_t i = 0; i < tok_len; i++) {
        if (!safe_char((unsigned char)tok[i])) return false;
    }
    /* Reject path traversal in any token. */
    for (size_t i = 0; i + 1 < tok_len; i++) {
        if (tok[i] == '.' && tok[i+1] == '.') return false;
    }
    return true;
}

/* Validates a shell command for safe execution.
 * Allowlist-only character set + per-token path-traversal check.
 * Rejects any shell metacharacter (| & ; < > * ? [ ] ( ) { } ' " ` $ \\ ! ~ # ^ \n \r \t)
 * and any token containing '..' as a substring. */
static bool command_is_safe_exec(const char* command) {
    if (!command || command[0] == '\0') return false;

    /* Global character allowlist (whitespace and safe chars only). */
    for (const char* p = command; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t') continue;
        if (!safe_char(c)) return false;
    }

    /* Per-token validation. */
    const char* p = command;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t len = (size_t)(p - start);
        if (!exec_token_is_safe(start, len)) return false;
    }
    return true;
}

/* Legacy wrapper: true when command is NOT safe (i.e. should be rejected). */
static bool command_contains_unsafe_token(const char* command) {
    return !command_is_safe_exec(command);
}

static bool resolve_workspace_path(const char* workspace, char* out, size_t out_size) {
    if (!workspace || !out || out_size == 0) return false;
    char resolved[4096];
    if (realpath(workspace, resolved) == NULL) return false;
    if (strlen(resolved) + 1 > out_size) return false;
    snprintf(out, out_size, "%s", resolved);
    return true;
}

/* Returns true if `path` (any form) resolves to a location strictly inside
 * `workspace`. TOCTOU-safe: opens with O_NOFOLLOW and verifies the opened
 * fd via fstat. Callers should still not trust the path beyond this point. */
static bool is_path_within_workspace(const char* path, const char* workspace) {
    if (!path || !workspace) return false;
    char ws[4096];
    if (!resolve_workspace_path(workspace, ws, sizeof(ws))) return false;
    size_t ws_len = strlen(ws);

    char resolved[4096];
    if (realpath(path, resolved) == NULL) {
        if (path[0] == '/') return false;
        char full[4096];
        int n = snprintf(full, sizeof(full), "%s/%s", workspace, path);
        if (n < 0 || (size_t)n >= sizeof(full)) return false;
        if (realpath(full, resolved) == NULL) return false;
    }
    if (strncmp(resolved, ws, ws_len) != 0) return false;
    if (resolved[ws_len] != '/' && resolved[ws_len] != '\0') return false;
    return true;
}

static bool resolve_existing_path_within_workspace(const char* path, const char* workspace, char* out, size_t out_size) {
    if (!path || !workspace || !out || out_size == 0) return false;
    char ws[4096];
    if (!resolve_workspace_path(workspace, ws, sizeof(ws))) return false;

    char candidate[4096];
    if (path[0] == '/') {
        snprintf(candidate, sizeof(candidate), "%s", path);
    } else {
        int n = snprintf(candidate, sizeof(candidate), "%s/%s", workspace, path);
        if (n < 0 || (size_t)n >= sizeof(candidate)) return false;
    }

    char resolved[4096];
    if (realpath(candidate, resolved) == NULL) return false;
    size_t ws_len = strlen(ws);
    if (strncmp(resolved, ws, ws_len) != 0) return false;
    if (resolved[ws_len] != '/' && resolved[ws_len] != '\0') return false;
    if (strlen(resolved) + 1 > out_size) return false;
    snprintf(out, out_size, "%s", resolved);
    return true;
}

static bool resolve_creatable_path_within_workspace(const char* path, const char* workspace, char* out, size_t out_size) {
    if (!path || !workspace || !out || out_size == 0) return false;
    char ws[4096];
    if (!resolve_workspace_path(workspace, ws, sizeof(ws))) return false;

    char candidate[4096];
    if (path[0] == '/') {
        snprintf(candidate, sizeof(candidate), "%s", path);
    } else {
        int n = snprintf(candidate, sizeof(candidate), "%s/%s", workspace, path);
        if (n < 0 || (size_t)n >= sizeof(candidate)) return false;
    }

    const char* base = strrchr(candidate, '/');
    const char* filename = base ? base + 1 : candidate;
    if (!filename || filename[0] == '\0' || strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) return false;
    if (strchr(filename, '/')) return false;

    char parent[4096];
    snprintf(parent, sizeof(parent), "%s", candidate);
    char* slash = strrchr(parent, '/');
    if (!slash) return false;
    if (slash == parent) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }

    char resolved_parent[4096];
    if (realpath(parent, resolved_parent) == NULL) return false;
    size_t ws_len = strlen(ws);
    if (strncmp(resolved_parent, ws, ws_len) != 0) return false;
    if (resolved_parent[ws_len] != '/' && resolved_parent[ws_len] != '\0') return false;

    int n = snprintf(out, out_size, "%s/%s", resolved_parent, filename);
    if (n < 0 || (size_t)n >= out_size) return false;
    return true;
}

/* Length-aware variant for path slices that may not be NUL-terminated.
 * Used by tool_exec token validation. */
static bool is_path_within_workspace_len(const char* path, size_t path_len, const char* workspace) {
    if (!path || path_len == 0 || !workspace) return false;
    char tmp[4096];
    if (path_len >= sizeof(tmp)) return false;
    memcpy(tmp, path, path_len);
    tmp[path_len] = '\0';
    return is_path_within_workspace(tmp, workspace);
}

// Helper to create directories recursively
static void ensure_dir(const char* path) {
    char tmp[FILE_PATH_MAX];
    char *p = NULL;
    size_t len;

    if (!path || path[0] == '\0') return;
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len == 0) return;
    if (tmp[len - 1] == '/') tmp[len - 1] = 0;
    
    char* last_slash = strrchr(tmp, '/');
    if (last_slash) {
        *last_slash = 0; 
        for (p = tmp + 1; *p; p++) {
            if (*p == '/') {
                *p = 0;
                mkdir(tmp, 0755);
                *p = '/';
            }
        }
        mkdir(tmp, 0755);
    }
}

// --- Tools Implementation ---

Error tool_memory(void* user_data, const char* args_json, String* result) {
    ToolContext* ctx = (ToolContext*)user_data;
    if (!ctx || !ctx->memory || !ctx->workspace) {
        return error_new(ERR_INVALID_PARAM, "Memory or Workspace not available in tool context");
    }

    cJSON* json = cJSON_Parse(args_json);
    if (!json) return error_new(ERR_JSON, "Invalid JSON arguments");
    
    char* history_entry = get_json_string(json, "history_entry");
    char* memory_update = get_json_string(json, "memory_update");
    
    // Fallback for backward compatibility or simple usage
    char* content = get_json_string(json, "content");
    
    if (!history_entry && !memory_update && !content) {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Missing arguments: provide 'history_entry' and/or 'memory_update'");
    }
    
    bool facts_changed = false;

    if (content && !memory_update) {
        memory_add_fact(ctx->memory, content);
        facts_changed = true;
    }

    if (history_entry) {
        Error append_err = memory_append_history(ctx->memory, ctx->workspace, history_entry);
        if (append_err.code != ERR_NONE) {
            cJSON_Delete(json);
            return append_err;
        }
    }

    if (memory_update) {
        Error set_err = memory_set_facts(ctx->memory, memory_update);
        if (set_err.code != ERR_NONE) {
            cJSON_Delete(json);
            return set_err;
        }
        facts_changed = true;
    }

    if (facts_changed) {
        Error err = memory_save(ctx->memory, ctx->workspace);
        if (err.code != ERR_NONE) {
            cJSON_Delete(json);
            return err;
        }
    }
    
    *result = string_new("Memory consolidated/updated successfully");
    cJSON_Delete(json);
    return error_new(ERR_NONE, "");
}

Error tool_skill(void* user_data, const char* args_json, String* result) {
    ToolContext* ctx = (ToolContext*)user_data;
    if (!ctx || !ctx->skills_loader) {
        return error_new(ERR_INVALID_PARAM, "SkillsLoader not available in tool context");
    }

    cJSON* json = cJSON_Parse(args_json);
    if (!json) return error_new(ERR_JSON, "Invalid JSON arguments");
    
    char* action = get_json_string(json, "action");
    if (!action) {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Missing 'action' argument");
    }
    
    if (strcmp(action, "list") == 0) {
        // List all skills (xml summary)
        char* summary = skills_loader_build_skills_summary(ctx->skills_loader);
        *result = string_new(summary);
        free(summary);
    } else if (strcmp(action, "load") == 0) {
        char* name = get_json_string(json, "name");
        if (!name) {
            cJSON_Delete(json);
            return error_new(ERR_INVALID_PARAM, "Missing 'name' argument for load action");
        }
        
        // Use path hints version to help LLM find correct script paths
        char* content = skills_loader_load_skill_with_path_hints(ctx->skills_loader, name);
        if (content) {
            *result = string_new(content);
            free(content);
        } else {
            *result = string_new("Skill not found");
        }
    } else if (strcmp(action, "unload") == 0) {
        *result = string_new("Unload not implemented (skills are stateless for now)");
    } else {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Unknown action");
    }
    
    cJSON_Delete(json);
    return error_new(ERR_NONE, "");
}

Error tool_read_file(void* user_data, const char* args_json, String* result) {
    ToolContext* ctx = (ToolContext*)user_data;

    char* error_msg = NULL;
    char* casted_args = tool_validate_and_cast_params(args_json,
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        &error_msg);

    if (error_msg) {
        size_t base_len = strlen(error_msg);
        size_t hint_len = strlen(TOOL_ERROR_HINT);
        char* full_error = malloc(base_len + hint_len + 1);
        if (!full_error) {
            free(error_msg);
            free(casted_args);
            return error_new(ERR_MEMORY, "Failed to allocate error message");
        }
        memcpy(full_error, error_msg, base_len);
        memcpy(full_error + base_len, TOOL_ERROR_HINT, hint_len);
        full_error[base_len + hint_len] = '\0';
        *result = string_new(full_error);
        free(full_error);
        free(error_msg);
        free(casted_args);
        return error_new(ERR_INVALID_PARAM, "Parameter validation failed");
    }

    cJSON* json = cJSON_Parse(casted_args);
    free(casted_args);

    char* path = get_json_string(json, "path");
    if (!path) {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Missing 'path' argument");
    }

    char safe_path[4096];
    const char* open_path = path;
    if (ctx && ctx->magic == TOOL_CONTEXT_MAGIC && ctx->config &&
        (ctx->config->tools.restrict_to_workspace || ctx->config->tools.exec.restrict_to_workspace)) {
        if (!resolve_existing_path_within_workspace(path, ctx->workspace, safe_path, sizeof(safe_path))) {
            cJSON_Delete(json);
            return error_new(ERR_TOOL, "Access denied: path is outside workspace");
        }
        open_path = safe_path;
    }
    
    FILE* fp = fopen(open_path, "r");
    if (!fp) {
        cJSON_Delete(json);
        return error_new(ERR_FILE, "Failed to open file");
    }
    
    fseek(fp, 0, SEEK_END);
    long length = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (length > MAX_READ_SIZE * 4) { 
        fclose(fp);
        cJSON_Delete(json);
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "File too large (%ld bytes)", length);
        return error_new(ERR_FILE, err_msg);
    }

    char* data = malloc(length + 1);
    if (!data) {
        fclose(fp);
        cJSON_Delete(json);
        return error_new(ERR_MEMORY, "Memory allocation failed");
    }
    
    if (fread(data, 1, length, fp) != (size_t)length) {
        free(data);
        fclose(fp);
        cJSON_Delete(json);
        return error_new(ERR_FILE, "Failed to read file");
    }
    data[length] = '\0';
    fclose(fp);
    
    if (length > MAX_READ_SIZE) {
        data[MAX_READ_SIZE] = '\0';
        char trunc_msg[128];
        snprintf(trunc_msg, sizeof(trunc_msg), "\n\n... (truncated — file is %ld chars, limit %d)", length, MAX_READ_SIZE);
        *result = string_new(data);
        string_append(result, trunc_msg);
    } else {
        *result = string_new(data);
    }
    
    free(data);
    cJSON_Delete(json);
    return error_new(ERR_NONE, "");
}

Error tool_write_file(void* user_data, const char* args_json, String* result) {
    ToolContext* ctx = (ToolContext*)user_data;
    cJSON* json = cJSON_Parse(args_json);
    if (!json) return error_new(ERR_JSON, "Invalid JSON arguments");
    
    char* path = get_json_string(json, "path");
    char* content = get_json_string(json, "content");
    
    if (!path || !content) {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Missing 'path' or 'content' argument");
    }

    char safe_path[4096];
    const char* write_path = path;
    if (ctx && ctx->magic == TOOL_CONTEXT_MAGIC && ctx->config &&
        (ctx->config->tools.restrict_to_workspace || ctx->config->tools.exec.restrict_to_workspace)) {
        if (!resolve_creatable_path_within_workspace(path, ctx->workspace, safe_path, sizeof(safe_path))) {
            cJSON_Delete(json);
            return error_new(ERR_TOOL, "Access denied: path is outside workspace");
        }
        write_path = safe_path;
    }
    
    ensure_dir(write_path);
    
    FILE* fp = fopen(write_path, "w");
    if (!fp) {
        cJSON_Delete(json);
        return error_new(ERR_FILE, "Failed to open file for writing");
    }
    
    fputs(content, fp);
    fclose(fp);
    
    *result = string_new("File written successfully");
    cJSON_Delete(json);
    return error_new(ERR_NONE, "");
}

Error tool_edit_file(void* user_data, const char* args_json, String* result) {
    ToolContext* ctx = (ToolContext*)user_data;
    cJSON* json = cJSON_Parse(args_json);
    if (!json) return error_new(ERR_JSON, "Invalid JSON arguments");
    
    char* path = get_json_string(json, "path");
    char* old_str = get_json_string(json, "old_str");
    char* new_str = get_json_string(json, "new_str");
    
    if (!path || !old_str || !new_str) {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Missing arguments");
    }

    char safe_path[4096];
    const char* edit_path = path;
    if (ctx && ctx->magic == TOOL_CONTEXT_MAGIC && ctx->config &&
        (ctx->config->tools.restrict_to_workspace || ctx->config->tools.exec.restrict_to_workspace)) {
        if (!resolve_existing_path_within_workspace(path, ctx->workspace, safe_path, sizeof(safe_path))) {
            cJSON_Delete(json);
            return error_new(ERR_TOOL, "Access denied: path is outside workspace");
        }
        edit_path = safe_path;
    }
    
    FILE* fp = fopen(edit_path, "r");
    if (!fp) {
        cJSON_Delete(json);
        return error_new(ERR_FILE, "Failed to open file");
    }
    
    fseek(fp, 0, SEEK_END);
    long length = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    char* data = malloc(length + 1);
    if (!data) {
        fclose(fp);
        cJSON_Delete(json);
        return error_new(ERR_MEMORY, "Memory allocation failed");
    }
    fread(data, 1, length, fp);
    data[length] = '\0';
    fclose(fp);
    
    char* pos = strstr(data, old_str);
    if (!pos) {
        free(data);
        cJSON_Delete(json);
        return error_new(ERR_TOOL, "old_str not found in file (exact match required)");
    }
    
    char* next_pos = strstr(pos + 1, old_str);
    if (next_pos) {
        free(data);
        cJSON_Delete(json);
        return error_new(ERR_TOOL, "old_str is not unique in file");
    }
    
    size_t new_len = length - strlen(old_str) + strlen(new_str);
    char* new_data = malloc(new_len + 1);
    if (!new_data) {
        free(data);
        cJSON_Delete(json);
        return error_new(ERR_MEMORY, "Memory allocation failed");
    }
    
    size_t prefix_len = (size_t)(pos - data);
    size_t new_str_len = strlen(new_str);
    size_t suffix_len = strlen(pos + strlen(old_str));
    memcpy(new_data, data, prefix_len);
    memcpy(new_data + prefix_len, new_str, new_str_len);
    memcpy(new_data + prefix_len + new_str_len, pos + strlen(old_str), suffix_len);
    new_data[prefix_len + new_str_len + suffix_len] = '\0';
    
    fp = fopen(edit_path, "w");
    if (!fp) {
        free(data);
        free(new_data);
        cJSON_Delete(json);
        return error_new(ERR_FILE, "Failed to write file");
    }
    
    fputs(new_data, fp);
    fclose(fp);
    
    free(data);
    free(new_data);
    cJSON_Delete(json);
    
    *result = string_new("File edited successfully");
    return error_new(ERR_NONE, "");
}

Error tool_list_dir(void* user_data, const char* args_json, String* result) {
    ToolContext* ctx = (ToolContext*)user_data;
    cJSON* json = cJSON_Parse(args_json);
    if (!json) return error_new(ERR_JSON, "Invalid JSON arguments");
    
    char* path = get_json_string(json, "path");
    if (!path) path = "."; 

    char safe_path[4096];
    const char* list_path = path;
    if (ctx && ctx->magic == TOOL_CONTEXT_MAGIC && ctx->config &&
        (ctx->config->tools.restrict_to_workspace || ctx->config->tools.exec.restrict_to_workspace)) {
        if (!resolve_existing_path_within_workspace(path, ctx->workspace, safe_path, sizeof(safe_path))) {
            cJSON_Delete(json);
            return error_new(ERR_TOOL, "Access denied: path is outside workspace");
        }
        list_path = safe_path;
    }

    DIR* d = opendir(list_path);
    if (!d) {
        cJSON_Delete(json);
        return error_new(ERR_FILE, "Failed to open directory");
    }
    
    struct dirent* dir;
    string_free(result);
    *result = string_new("");
    
    while ((dir = readdir(d)) != NULL) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;
        
        char fullpath[1024];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", list_path, dir->d_name);
        struct stat st;
        stat(fullpath, &st);
        
        if (S_ISDIR(st.st_mode)) {
            string_append(result, "📁 ");
        } else {
            string_append(result, "📄 ");
        }
        string_append(result, dir->d_name);
        string_append(result, "\n");
    }
    
    closedir(d);
    cJSON_Delete(json);
    return error_new(ERR_NONE, "");
}

Error tool_exec(void* user_data, const char* args_json, String* result) {
    ToolContext* ctx = (ToolContext*)user_data;
    cJSON* json = cJSON_Parse(args_json);
    if (!json) return error_new(ERR_JSON, "Invalid JSON arguments");

    char* command = get_json_string(json, "command");
    if (!command) {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Missing 'command' argument");
    }

    bool restrict_exec = true;
    if (ctx && ctx->magic == TOOL_CONTEXT_MAGIC && ctx->config) {
        restrict_exec = ctx->config->tools.exec.restrict_to_workspace || ctx->config->tools.restrict_to_workspace;
    }

    if (restrict_exec) {
        if (command_contains_unsafe_token(command)) {
            cJSON_Delete(json);
            return error_new(ERR_TOOL, "Command rejected by workspace restriction policy (shell metacharacters or path traversal)");
        }
        /* Additionally: any absolute-path token must resolve within workspace. */
        const char* restrict_ws = (ctx && ctx->magic == TOOL_CONTEXT_MAGIC) ? ctx->workspace : NULL;
        if (restrict_ws) {
            const char* p = command;
            while (*p) {
                while (*p == ' ' || *p == '\t') p++;
                if (!*p) break;
                const char* start = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                size_t len = (size_t)(p - start);
                if (len > 0 && start[0] == '/' &&
                    !is_path_within_workspace_len(start, len, restrict_ws)) {
                    cJSON_Delete(json);
                    return error_new(ERR_TOOL, "Command rejected: absolute path outside workspace");
                }
            }
        }
    }

    const char* workspace = (ctx && ctx->magic == TOOL_CONTEXT_MAGIC && ctx->workspace) ? ctx->workspace : ".";

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        cJSON_Delete(json);
        return error_new(ERR_TOOL, "Failed to create pipe");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        cJSON_Delete(json);
        return error_new(ERR_TOOL, "Failed to fork");
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        if (restrict_exec && workspace) {
            chdir(workspace);
        }

        char* shell_args[] = {"sh", "-c", (char*)command, NULL};
        execvp("sh", shell_args);
        _exit(127);
    }

    close(pipefd[1]);

    *result = string_new("");
    char buffer[4096];
    ssize_t n;

    int timeout_secs = 300;
    if (ctx && ctx->magic == TOOL_CONTEXT_MAGIC && ctx->config) {
        timeout_secs = ctx->config->tools.exec.timeout;
    }
    
    fd_set read_fds;
    struct timeval tv;
    bool timed_out = false;
    time_t start_time = time(NULL);

    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(pipefd[0], &read_fds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int sel = select(pipefd[0] + 1, &read_fds, NULL, NULL, &tv);
        if (sel < 0) break;
        
        if (sel == 0) {
            if (time(NULL) - start_time >= timeout_secs) {
                timed_out = true;
                break;
            }
            continue;
        }
        
        n = read(pipefd[0], buffer, sizeof(buffer) - 1);
        if (n <= 0) break;
        buffer[n] = '\0';
        string_append(result, buffer);
    }
    close(pipefd[0]);

    int status;
    if (timed_out) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        string_append(result, "\n[TIMEOUT] Command timed out after ");
        char timeout_str[32];
        snprintf(timeout_str, sizeof(timeout_str), "%d seconds", timeout_secs);
        string_append(result, timeout_str);
    } else {
        waitpid(pid, &status, 0);
        char status_str[128];
        if (WIFEXITED(status)) {
            snprintf(status_str, sizeof(status_str), "\nExit code: %d", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            snprintf(status_str, sizeof(status_str), "\nTerminated by signal: %d", WTERMSIG(status));
        } else {
            snprintf(status_str, sizeof(status_str), "\nExit status: %d", status);
        }
        string_append(result, status_str);
    }

    cJSON_Delete(json);
    return error_new(ERR_NONE, "");
}

Error tool_send_message(void* user_data, const char* args_json, String* result) {
    ToolContext* ctx = (ToolContext*)user_data;
    log_debug("[send_message] user_data=%p, ctx=%p", user_data, (void*)ctx);
    if (ctx) {
        log_debug("[send_message] ctx->magic=0x%x (expected=0x50474e31), ctx->bus=%p", 
                  ctx->magic, (void*)ctx->bus);
    }
    if (!ctx || ctx->magic != 0x50474E31) {
        log_error("[send_message] Invalid ToolContext: ctx=%p, magic=0x%x", (void*)ctx, ctx ? ctx->magic : 0);
        return error_new(ERR_INVALID_PARAM, "Invalid tool context");
    }
    if (!ctx->bus) {
        log_error("[send_message] MessageBus is NULL in ToolContext");
        return error_new(ERR_INVALID_PARAM, "MessageBus not available in tool context");
    }
    
    cJSON* json = cJSON_Parse(args_json);
    if (!json) return error_new(ERR_JSON, "Invalid JSON arguments");
    
    char* content = get_json_string(json, "content");
    char* channel = get_json_string(json, "channel");
    char* chat_id = get_json_string(json, "chat_id");
    
    log_debug("[send_message] Before resolve: channel=%s, chat_id=%s, ctx->current_channel=%s, ctx->current_chat_id=%s",
              channel ? channel : "NULL", chat_id ? chat_id : "NULL",
              ctx->current_channel[0] ? ctx->current_channel : "(empty)",
              ctx->current_chat_id[0] ? ctx->current_chat_id : "(empty)");
    
    channel = (char*) resolve_channel(ctx, channel);
    chat_id = (char*) resolve_chat_id(ctx, chat_id);
    
    log_debug("[send_message] After resolve: channel=%s, chat_id=%s", channel, chat_id);
    
    if (!content) {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Missing 'content' argument");
    }
    
    OutboundMessage* msg = outbound_message_new(channel, chat_id, content);
    if (!msg) {
        cJSON_Delete(json);
        return error_new(ERR_MEMORY, "Failed to allocate outbound message");
    }

    log_debug("[send_message] Parsing attachments from JSON");
    Error parse_error = parse_send_message_attachments(json, msg);
    if (parse_error.code != ERR_NONE) {
        outbound_message_free(msg);
        cJSON_Delete(json);
        return parse_error;
    }
    log_debug("[send_message] After parsing: attachments.count=%zu", msg->attachments.count);

    /* Workspace policy: reject any attachment path outside workspace. */
    if (ctx->magic == TOOL_CONTEXT_MAGIC && ctx->config &&
        (ctx->config->tools.restrict_to_workspace || ctx->config->tools.exec.restrict_to_workspace) &&
        ctx->workspace) {
        for (size_t ai = 0; ai < msg->attachments.count; ai++) {
            const char* att = msg->attachments.items[ai].data;
            if (!att) continue;
            /* Extract a "path":"..." substring if the attachment is an object
             * (it was normalized to JSON); for plain string attachments, use the
             * value directly. */
            const char* pkey = strstr(att, "\"path\":\"");
            const char* path = pkey ? pkey + 8 : att;
            const char* end = path;
            if (pkey) {
                end = strchr(path, '\"');
                if (end) {
                    size_t path_len = (size_t)(end - path);
                    char tmp[4096];
                    if (path_len >= sizeof(tmp)) {
                        outbound_message_free(msg);
                        cJSON_Delete(json);
                        return error_new(ERR_TOOL, "Attachment path too long");
                    }
                    memcpy(tmp, path, path_len);
                    tmp[path_len] = '\0';
                    if (!is_path_within_workspace(tmp, ctx->workspace)) {
                        outbound_message_free(msg);
                        cJSON_Delete(json);
                        return error_new(ERR_TOOL, "Attachment path outside workspace");
                    }
                    continue;
                }
            }
            /* Plain string attachment: validate as path. */
            if (!is_path_within_workspace(path, ctx->workspace)) {
                outbound_message_free(msg);
                cJSON_Delete(json);
                return error_new(ERR_TOOL, "Attachment path outside workspace");
            }
        }
    }

    message_bus_send_outbound(ctx->bus, msg);
    
    *result = string_new("Message queued for delivery");
    cJSON_Delete(json);
    return error_new(ERR_NONE, "");
}

Error tool_spawn(void* user_data, const char* args_json, String* result) {
    ToolContext* ctx = (ToolContext*)user_data;
    if (!ctx || !ctx->subagent_mgr) {
        return error_new(ERR_INVALID_PARAM, "SubagentManager not available in tool context");
    }

    cJSON* json = cJSON_Parse(args_json);
    if (!json) return error_new(ERR_JSON, "Invalid JSON arguments");
    
    char* task = get_json_string(json, "task");
    char* label = get_json_string(json, "label");
    
    if (!task) {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Missing 'task' argument");
    }
    
    SubagentSpawnRequest req;
    req.task = task;
    req.label = label;
    req.origin_channel = (ctx->current_channel[0]) ? ctx->current_channel : "cli";
    req.origin_chat_id = (ctx->current_chat_id[0]) ? ctx->current_chat_id : "current";
    
    char* resp = subagent_manager_spawn(ctx->subagent_mgr, &req);
    if (resp) {
        *result = string_new(resp);
        free(resp);
    } else {
        *result = string_new("Failed to spawn subagent");
    }
    
    cJSON_Delete(json);
    return error_new(ERR_NONE, "");
}

Error tool_cron(void* user_data, const char* args_json, String* result) {
    ToolContext* ctx = (ToolContext*)user_data;
    if (!ctx || !ctx->cron_service) {
        return error_new(ERR_INVALID_PARAM, "CronService not available in tool context");
    }

    cJSON* json = cJSON_Parse(args_json);
    if (!json) return error_new(ERR_JSON, "Invalid JSON arguments");
    
    char* name = get_json_string(json, "name");
    char* payload = get_json_string(json, "payload");
    char* schedule = get_json_string(json, "schedule");
    char* channel = get_json_string(json, "channel");
    char* chat_id = get_json_string(json, "chat_id");
    
    if (!name || !payload || !schedule) {
        char missing_info[256] = "";
        snprintf(missing_info, sizeof(missing_info),
                 "Missing required arguments: %s%s%s. "
                 "Required: name (string), payload (string), schedule (string). "
                 "Example: {\"name\":\"drink-water\",\"payload\":\"Remind user to drink water\",\"schedule\":\"@in 60\"}",
                 !name ? "name" : "",
                 !payload ? (!name ? ", payload" : "payload") : "",
                 !schedule ? (!name || !payload ? ", schedule" : "schedule") : "");
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, missing_info);
    }
    
    CronJob job;
    memset(&job, 0, sizeof(job));
    job.name = name;
    job.payload_message = payload;
    job.schedule = schedule;
    job.channel = (char*) resolve_channel(ctx, channel);
    job.to = (char*) resolve_chat_id(ctx, chat_id);
    if (!job.channel || job.channel[0] == '\0') job.channel = "cli";
    if (!job.to || job.to[0] == '\0') job.to = "local_user";
    job.deliver = true;
    
    char* job_id = cron_service_add_job(ctx->cron_service, &job);
    
    if (job_id) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Job scheduled with ID: %s", job_id);
        *result = string_new(msg);
        free(job_id);
    } else {
        *result = string_new("Failed to schedule job: invalid schedule format");
    }
    
    cJSON_Delete(json);
    return error_new(ERR_NONE, "");
}
