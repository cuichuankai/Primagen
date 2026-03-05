#include "tools_impl.h"
#include "../vendor/cJSON/cJSON.h"
#include "../bus/message_bus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <curl/curl.h>
#include <ctype.h>

#define MAX_READ_SIZE 128000

// Helper struct for curl response
struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;
    
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(!ptr) return 0; // Out of memory
    
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    
    return realsize;
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
    tool_registry_register(reg, "cron", "Schedule a job", 
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"payload\":{\"type\":\"string\"},\"schedule\":{\"type\":\"string\"}},\"required\":[\"name\",\"payload\",\"schedule\"]}", 
        tool_cron, ctx);
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
    
    CURL *curl = curl_easy_init();
    if (!curl) {
        cJSON_Delete(json);
        return error_new(ERR_NETWORK, "Failed to init CURL");
    }
    
    char url[512];
    char* encoded_query = curl_easy_escape(curl, query, 0);
    snprintf(url, sizeof(url), "https://api.search.brave.com/res/v1/web/search?q=%s&count=%d", encoded_query, count);
    curl_free(encoded_query);
    
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "X-Subscription-Token: %s", api_key);
    headers = curl_slist_append(headers, auth_header);
    
    struct MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        free(chunk.memory);
        cJSON_Delete(json);
        return error_new(ERR_NETWORK, curl_easy_strerror(res));
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
    
    CURL *curl = curl_easy_init();
    if (!curl) {
        cJSON_Delete(json);
        return error_new(ERR_NETWORK, "Failed to init CURL");
    }
    
    struct MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_7_2) AppleWebKit/537.36");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        free(chunk.memory);
        cJSON_Delete(json);
        return error_new(ERR_NETWORK, curl_easy_strerror(res));
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
    
    if (!channel) channel = "cli";
    if (!chat_id) chat_id = "current";
    
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
    req.origin_channel = "cli"; // simplified
    req.origin_chat_id = "current"; // simplified
    
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
    
    if (!name || !payload || !schedule) {
        cJSON_Delete(json);
        return error_new(ERR_INVALID_PARAM, "Missing arguments");
    }
    
    CronJob job;
    memset(&job, 0, sizeof(job));
    job.name = name;
    job.payload_message = payload;
    job.schedule = schedule;
    // Defaults
    job.channel = "cli";
    job.to = "local_user";
    job.deliver = true;
    
    char* job_id = cron_service_add_job(ctx->cron_service, &job);
    
    if (job_id) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Job scheduled with ID: %s", job_id);
        *result = string_new(msg);
        free(job_id);
    } else {
        *result = string_new("Failed to schedule job");
    }
    
    cJSON_Delete(json);
    return error_new(ERR_NONE, "");
}
