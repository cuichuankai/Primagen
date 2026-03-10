#include "tools_impl.h"
#include "../vendor/cJSON/cJSON.h"
#include "../bus/message_bus.h"
#include "../vendor/mongoose/mongoose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>

#define MAX_READ_SIZE 128000

// Helper struct for mongoose response
struct MemoryStruct {
    char *memory;
    size_t size;
    bool done;
};

static void fn(struct mg_connection *c, int ev, void *ev_data) {
  struct MemoryStruct *ms = (struct MemoryStruct *) c->fn_data;
  if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    size_t new_size = ms->size + hm->body.len;
    ms->memory = realloc(ms->memory, new_size + 1);
    // mongoose 7.x uses buf/len for mg_str but some versions use ptr.
    // Let's check mongoose.h. It seems to be 'buf' or 'ptr' depending on version.
    // If 'ptr' error, likely it is 'buf' (const char* buf).
    memcpy(ms->memory + ms->size, hm->body.buf, hm->body.len);
    ms->size = new_size;
    ms->memory[ms->size] = '\0';
    c->is_closing = 1;
    ms->done = true;
  } else if (ev == MG_EV_ERROR) {
      ms->done = true;
  }
}

// Helper to get string from JSON
static char* get_json_string(cJSON* root, const char* key) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return NULL;
}

static int get_json_int(cJSON* root, const char* key, int default_val) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return default_val;
}

static bool is_placeholder_value(const char* s) {
    if (!s || s[0] == '\0') return true;
    if (strcmp(s, "current") == 0 || strcmp(s, "chat") == 0 ||
        strcmp(s, "user") == 0 || strcmp(s, "assistant") == 0) return true;
    if (strncmp(s, "_user_", 6) == 0 || strncmp(s, "_assistant_", 11) == 0) return true;
    return false;
}

static bool is_known_channel(const char* s) {
    return s && (strcmp(s, "console") == 0 || strcmp(s, "telegram") == 0 ||
                 strcmp(s, "email") == 0 || strcmp(s, "discord") == 0 ||
                 strcmp(s, "slack") == 0 || strcmp(s, "dingtalk") == 0 ||
                 strcmp(s, "feishu") == 0 || strcmp(s, "system") == 0);
}

static const char* resolve_channel(ToolContext* ctx, const char* channel) {
    if (!is_placeholder_value(channel) && is_known_channel(channel)) return channel;
    if (ctx && ctx->current_channel && ctx->current_channel[0]) return ctx->current_channel;
    return "cli";
}

static const char* resolve_chat_id(ToolContext* ctx, const char* chat_id) {
    if (!is_placeholder_value(chat_id)) return chat_id;
    if (ctx && ctx->current_chat_id && ctx->current_chat_id[0]) return ctx->current_chat_id;
    return "current";
}

void tool_context_set_route(ToolContext* ctx, const char* channel, const char* chat_id) {
    if (!ctx) return;
    ctx->current_channel = channel;
    ctx->current_chat_id = chat_id;
}

// Helper to create directories recursively
static void ensure_dir(const char* path) {
    char tmp[512];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
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

// Simple HTML strip tags
static void strip_tags(char* src, char* dst) {
    int in_tag = 0;
    while (*src) {
        if (*src == '<') {
            in_tag = 1;
        } else if (*src == '>') {
            in_tag = 0;
        } else if (!in_tag) {
            *dst++ = *src;
        }
        src++;
    }
    *dst = 0;
}

// --- Tools Implementation ---

void register_all_tools(ToolRegistry* reg, ToolContext* ctx) {
    tool_registry_register(reg, "read_file", "Read file content", 
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}", 
        tool_read_file, ctx);
    tool_registry_register(reg, "write_file", "Write file content", 
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}", 
        tool_write_file, ctx);
    tool_registry_register(reg, "edit_file", "Edit file content", 
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old_str\":{\"type\":\"string\"},\"new_str\":{\"type\":\"string\"}},\"required\":[\"path\",\"old_str\",\"new_str\"]}", 
        tool_edit_file, ctx);
    tool_registry_register(reg, "list_dir", "List directory contents", 
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}", 
        tool_list_dir, ctx);
    tool_registry_register(reg, "exec", "Execute shell command", 
        "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}", 
        tool_exec, ctx);
    tool_registry_register(reg, "web_search", "Search the web", 
        "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"count\":{\"type\":\"integer\"}},\"required\":[\"query\"]}", 
        tool_web_search, ctx);
    tool_registry_register(reg, "web_fetch", "Fetch URL content", 
        "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}", 
        tool_web_fetch, ctx);
    tool_registry_register(reg, "send_message", "Send message to user", 
        "{\"type\":\"object\",\"properties\":{\"content\":{\"type\":\"string\"}},\"required\":[\"content\"]}", 
        tool_send_message, ctx);
    tool_registry_register(reg, "spawn_subagent", "Spawn subagent", 
        "{\"type\":\"object\",\"properties\":{\"task\":{\"type\":\"string\"},\"label\":{\"type\":\"string\"}},\"required\":[\"task\"]}", 
        tool_spawn, ctx);
    tool_registry_register(reg, "cron", "Schedule a job. Formats: '@every N', '@in N', '@at N', or daily cron 'M H * * *' in local time", 
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"payload\":{\"type\":\"string\"},\"schedule\":{\"type\":\"string\"},\"channel\":{\"type\":\"string\"},\"chat_id\":{\"type\":\"string\"}},\"required\":[\"name\",\"payload\",\"schedule\"]}", 
        tool_cron, ctx);
    tool_registry_register(reg, "skill", "Manage skills", 
        "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list\",\"load\",\"unload\"]},\"name\":{\"type\":\"string\"}},\"required\":[\"action\"]}", 
        tool_skill, ctx);
    tool_registry_register(reg, "memory", "Manage long-term memory. Use this to consolidate conversation history into persistent memory.", 
        "{\"type\":\"object\",\"properties\":{\"history_entry\":{\"type\":\"string\",\"description\":\"A paragraph summarizing key events/decisions. Start with [YYYY-MM-DD HH:MM].\"},\"memory_update\":{\"type\":\"string\",\"description\":\"Full updated long-term memory content (facts). Return unchanged if no new facts.\"}},\"required\":[\"history_entry\"]}", 
        tool_memory, ctx);
}

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
    
    // Handle simple content (treat as fact)
    if (content && !memory_update) {
        memory_add_fact(ctx->memory, content);
    }
    
    // Handle history entry
    if (history_entry) {
        memory_add_history(ctx->memory, history_entry);
    }
    
    // Handle memory update (full replacement/update of facts)
    if (memory_update) {

        string_free(&ctx->memory->memory_md);
        ctx->memory->memory_md = string_new(memory_update);
    }
    
    // Persist immediately
    Error err = memory_save(ctx->memory, ctx->workspace);
    if (err.code != ERR_NONE) {
        cJSON_Delete(json);
        return err;
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
    cJSON* json = cJSON_Parse(args_json);
    if (!json) return error_new(ERR_JSON, "Invalid JSON arguments");
    
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
    (void)user_data;
    cJSON* json = cJSON_Parse(args_json);
    if (!json) return error_new(ERR_JSON, "Invalid JSON arguments");
    
    char* command = get_json_string(json, "command");
    if (!command) {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Missing 'command' argument");
    }
    
    FILE* fp = popen(command, "r");
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
    char status_str[64];
    snprintf(status_str, sizeof(status_str), "\nExit code: %d", status);
    string_append(result, status_str);
    
    cJSON_Delete(json);
    return error_new(ERR_NONE, "");
}

Error tool_web_search(void* user_data, const char* args_json, String* result) {
    (void)user_data;
    cJSON* json = cJSON_Parse(args_json);
    if (!json) return error_new(ERR_JSON, "Invalid JSON arguments");
    
    char* query = get_json_string(json, "query");
    int count = get_json_int(json, "count", 5);
    if (count < 1) count = 1;
    if (count > 10) count = 10;
    
    if (!query) {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Missing 'query' argument");
    }
    
    const char* api_key = getenv("BRAVE_API_KEY");
    if (!api_key) {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "BRAVE_API_KEY not set");
    }
    
    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';
    
    mg_mgr_init(&mgr);

    char url[1024];
    // Simple URL encoding manually or use mg_url_encode if available, 
    // but mongoose doesn't expose a simple string encode helper easily.
    // Let's just put query as is for now or implement simple encoder.
    // Actually, Brave API expects query param.
    // We should encode spaces at least.
    
    // Quick and dirty URL encoder for spaces
    char encoded_query[2048] = {0};
    size_t qlen = strlen(query);
    size_t eidx = 0;
    for (size_t i=0; i<qlen && eidx < sizeof(encoded_query)-4; i++) {
        if (isalnum(query[i]) || query[i] == '-' || query[i] == '_' || query[i] == '.' || query[i] == '~') {
            encoded_query[eidx++] = query[i];
        } else {
            snprintf(encoded_query + eidx, 4, "%%%02X", (unsigned char)query[i]);
            eidx += 3;
        }
    }
    
    snprintf(url, sizeof(url), "https://api.search.brave.com/res/v1/web/search?q=%s&count=%d", encoded_query, count);
    
    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        cJSON_Delete(json);
        mg_mgr_free(&mgr);
        free(chunk.memory);
        return error_new(ERR_NETWORK, "Failed to connect to Search provider");
    }
    
    // struct mg_tls_opts opts = {0};
    // opts.ca = mg_str("ca.pem");

    struct mg_str host = mg_url_host(url);
    mg_printf(c, 
        "GET %s HTTP/1.0\r\n"
        "Host: %.*s\r\n"
        "Accept: application/json\r\n"
        "X-Subscription-Token: %s\r\n"
        "\r\n",
        mg_url_uri(url), 
        (int)host.len, host.buf,
        api_key
    );

    while (!chunk.done) mg_mgr_poll(&mgr, 1000);
    mg_mgr_free(&mgr);
    
    if (chunk.size == 0) {
        free(chunk.memory);
        cJSON_Delete(json);
        return error_new(ERR_NETWORK, "Empty response from Search provider");
    }

    cJSON* resp = cJSON_Parse(chunk.memory);
    free(chunk.memory);
    
    if (!resp) {
        cJSON_Delete(json);
        return error_new(ERR_JSON, "Failed to parse search response");
    }
    
    cJSON* web = cJSON_GetObjectItem(resp, "web");
    cJSON* results = cJSON_GetObjectItem(web, "results");
    
    *result = string_new("");
    char line[1024];
    snprintf(line, sizeof(line), "Results for: %s\n", query);
    string_append(result, line);
    
    int i = 1;
    cJSON* item;
    cJSON_ArrayForEach(item, results) {
        cJSON* title = cJSON_GetObjectItem(item, "title");
        cJSON* url_item = cJSON_GetObjectItem(item, "url");
        cJSON* desc = cJSON_GetObjectItem(item, "description");
        
        snprintf(line, sizeof(line), "%d. %s\n   %s\n", i++, 
                 title ? title->valuestring : "",
                 url_item ? url_item->valuestring : "");
        string_append(result, line);
        if (desc && desc->valuestring) {
            string_append(result, "   ");
            string_append(result, desc->valuestring);
            string_append(result, "\n");
        }
    }
    
    if (i == 1) {
        *result = string_new("No results found.");
    }
    
    cJSON_Delete(resp);
    cJSON_Delete(json);
    return error_new(ERR_NONE, "");
}

Error tool_web_fetch(void* user_data, const char* args_json, String* result) {
    (void)user_data;
    cJSON* json = cJSON_Parse(args_json);
    if (!json) return error_new(ERR_JSON, "Invalid JSON arguments");
    
    char* url = get_json_string(json, "url");
    if (!url) {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Missing 'url' argument");
    }
    
    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';
    
    mg_mgr_init(&mgr);
    
    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        cJSON_Delete(json);
        mg_mgr_free(&mgr);
        free(chunk.memory);
        return error_new(ERR_NETWORK, "Failed to connect to URL");
    }
    
    // struct mg_tls_opts opts = {0};
    // opts.ca = mg_str("ca.pem");

    struct mg_str host = mg_url_host(url);
    mg_printf(c, 
        "GET %s HTTP/1.0\r\n"
        "Host: %.*s\r\n"
        "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 14_7_2) AppleWebKit/537.36\r\n"
        "\r\n",
        mg_url_uri(url), 
        (int)host.len, host.buf
    );
    
    while (!chunk.done) mg_mgr_poll(&mgr, 1000);
    mg_mgr_free(&mgr);
    
    if (chunk.size == 0) {
        free(chunk.memory);
        cJSON_Delete(json);
        return error_new(ERR_NETWORK, "Empty response or network error");
    }
    
    char* text = malloc(chunk.size + 1);
    if (!text) {
        free(chunk.memory);
        cJSON_Delete(json);
        return error_new(ERR_MEMORY, "Out of memory");
    }
    
    strip_tags(chunk.memory, text);
    free(chunk.memory);
    
    *result = string_new(text);
    free(text);
    cJSON_Delete(json);
    return error_new(ERR_NONE, "");
}

Error tool_send_message(void* user_data, const char* args_json, String* result) {
    ToolContext* ctx = (ToolContext*)user_data;
    if (!ctx || !ctx->bus) {
        return error_new(ERR_INVALID_PARAM, "MessageBus not available in tool context");
    }
    
    cJSON* json = cJSON_Parse(args_json);
    if (!json) return error_new(ERR_JSON, "Invalid JSON arguments");
    
    char* content = get_json_string(json, "content");
    char* channel = get_json_string(json, "channel");
    char* chat_id = get_json_string(json, "chat_id");
    
    channel = (char*) resolve_channel(ctx, channel);
    chat_id = (char*) resolve_chat_id(ctx, chat_id);
    
    if (!content) {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Missing 'content' argument");
    }
    
    OutboundMessage* msg = outbound_message_new(channel, chat_id, content);
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
    req.origin_channel = (ctx->current_channel && ctx->current_channel[0]) ? ctx->current_channel : "cli";
    req.origin_chat_id = (ctx->current_chat_id && ctx->current_chat_id[0]) ? ctx->current_chat_id : "current";
    
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
