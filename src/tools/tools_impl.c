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

#define MAX_READ_SIZE 128000
#define TOOL_CONTEXT_MAGIC 0x50474E31

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

static bool is_placeholder_value(const char* s) {
    if (!s || s[0] == '\0') return true;
    if (strcmp(s, "current") == 0 || strcmp(s, "chat") == 0 ||
        strcmp(s, "user") == 0 || strcmp(s, "assistant") == 0) return true;
    if (strncmp(s, "_user_", 6) == 0 || strncmp(s, "_assistant_", 11) == 0) return true;
    return false;
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
}

static bool command_contains_unsafe_token(const char* command) {
    if (!command) return true;
    const char* tokens[] = {"&&", "||", ";", "|", ">", "<", "`", "$(", "\n", "\r"};
    size_t n = sizeof(tokens) / sizeof(tokens[0]);
    for (size_t i = 0; i < n; i++) {
        if (strstr(command, tokens[i])) return true;
    }
    if (strncmp(command, "/", 1) == 0) return true;
    if (strstr(command, " ../") || strstr(command, "../")) return true;
    return false;
}

static char* shell_escape_single_quotes(const char* input) {
    if (!input) return strdup("");
    size_t len = strlen(input);
    size_t extra = 0;
    for (size_t i = 0; i < len; i++) {
        if (input[i] == '\'') extra += 3;
    }
    char* out = malloc(len + extra + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (input[i] == '\'') {
            out[j++] = '\'';
            out[j++] = '\\';
            out[j++] = '\'';
            out[j++] = '\'';
        } else {
            out[j++] = input[i];
        }
    }
    out[j] = '\0';
    return out;
}

// Helper to create directories recursively
static void ensure_dir(const char* path) {
    char tmp[512];
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
        
        char* content = skills_loader_load_skill(ctx->skills_loader, name);
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
    (void)user_data;

    // Validate and cast parameters
    char* error_msg = NULL;
    char* casted_args = tool_validate_and_cast_params(args_json,
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        &error_msg);

    if (error_msg) {
        char* full_error = malloc(strlen(error_msg) + strlen(TOOL_ERROR_HINT) + 1);
        if (!full_error) {
            free(error_msg);
            free(casted_args);
            return error_new(ERR_MEMORY, "Failed to allocate error message");
        }
        strcpy(full_error, error_msg);
        strcat(full_error, TOOL_ERROR_HINT);
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
    
    FILE* fp = fopen(path, "r");
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
    (void)user_data;
    cJSON* json = cJSON_Parse(args_json);
    if (!json) return error_new(ERR_JSON, "Invalid JSON arguments");
    
    char* path = get_json_string(json, "path");
    char* content = get_json_string(json, "content");
    
    if (!path || !content) {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Missing 'path' or 'content' argument");
    }
    
    ensure_dir(path);
    
    FILE* fp = fopen(path, "w");
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
    (void)user_data;
    cJSON* json = cJSON_Parse(args_json);
    if (!json) return error_new(ERR_JSON, "Invalid JSON arguments");
    
    char* path = get_json_string(json, "path");
    char* old_str = get_json_string(json, "old_str");
    char* new_str = get_json_string(json, "new_str");
    
    if (!path || !old_str || !new_str) {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Missing arguments");
    }
    
    FILE* fp = fopen(path, "r");
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
    
    size_t prefix_len = pos - data;
    strncpy(new_data, data, prefix_len);
    strcpy(new_data + prefix_len, new_str);
    strcpy(new_data + prefix_len + strlen(new_str), pos + strlen(old_str));
    
    fp = fopen(path, "w");
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
    (void)user_data;
    cJSON* json = cJSON_Parse(args_json);
    if (!json) return error_new(ERR_JSON, "Invalid JSON arguments");
    
    char* path = get_json_string(json, "path");
    if (!path) path = "."; 
    
    DIR* d = opendir(path);
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
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, dir->d_name);
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

    char* shell_cmd = NULL;
    bool restrict_exec = true;
    if (ctx && ctx->magic == TOOL_CONTEXT_MAGIC && ctx->config) {
        restrict_exec = ctx->config->tools.exec.restrict_to_workspace || ctx->config->tools.restrict_to_workspace;
    }
    if (restrict_exec) {
        if (command_contains_unsafe_token(command)) {
            cJSON_Delete(json);
            return error_new(ERR_TOOL, "Command rejected by workspace restriction policy");
        }
        const char* workspace = (ctx && ctx->magic == TOOL_CONTEXT_MAGIC && ctx->workspace) ? ctx->workspace : ".";
        char* escaped_workspace = shell_escape_single_quotes(workspace);
        if (!escaped_workspace) {
            cJSON_Delete(json);
            return error_new(ERR_MEMORY, "Memory allocation failed");
        }
        asprintf(&shell_cmd, "cd '%s' && %s 2>&1", escaped_workspace, command);
        free(escaped_workspace);
    } else {
        asprintf(&shell_cmd, "%s 2>&1", command);
    }
    if (!shell_cmd) {
        cJSON_Delete(json);
        return error_new(ERR_MEMORY, "Memory allocation failed");
    }

    FILE* fp = popen(shell_cmd, "r");
    free(shell_cmd);

    if (!fp) {
        cJSON_Delete(json);
        return error_new(ERR_TOOL, "Failed to execute command");
    }

    char buffer[1024];
    *result = string_new("");

    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        string_append(result, buffer);
    }

    int status = pclose(fp);
    char status_str[128];
    if (WIFEXITED(status)) {
        snprintf(status_str, sizeof(status_str), "\nExit code: %d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        snprintf(status_str, sizeof(status_str), "\nTerminated by signal: %d", WTERMSIG(status));
    } else {
        snprintf(status_str, sizeof(status_str), "\nExit status: %d", status);
    }
    string_append(result, status_str);

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
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Missing arguments");
    }
    
    CronJob job;
    memset(&job, 0, sizeof(job));
    job.name = name;
    job.payload_message = payload;
    job.schedule = schedule;
    // Defaults or user provided
    job.channel = (char*) resolve_channel(ctx, channel);
    job.to = (char*) resolve_chat_id(ctx, chat_id);
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
