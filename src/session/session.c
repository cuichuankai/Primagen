#include "session.h"
#include "../include/common.h"
#include "../include/message.h"
#include <dirent.h>
#include <sys/stat.h>

SessionManager* session_manager_new(const char* workspace_path) {
    SessionManager* mgr = malloc(sizeof(SessionManager));
    if (!mgr) return NULL;
    mgr->count = 0;
    mgr->capacity = 8;
    mgr->sessions = malloc(mgr->capacity * sizeof(Session*));
    mgr->workspace_path = string_new(workspace_path);
    // Create sessions directory if not exists
    char path[512];
    snprintf(path, sizeof(path), "%s/sessions", workspace_path);
    mkdir(path, 0755);
    return mgr;
}

void session_manager_free(SessionManager* mgr) {
    if (!mgr) return;
    for (size_t i = 0; i < mgr->count; i++) {
        // Free session
        string_free(&mgr->sessions[i]->key);
        for (size_t j = 0; j < mgr->sessions[i]->messages.count; j++) {
            Message* msg = *(Message**)dynamic_array_get(&mgr->sessions[i]->messages, j);
            message_free(msg);
        }
        dynamic_array_free(&mgr->sessions[i]->messages);
        free(mgr->sessions[i]);
    }
    free(mgr->sessions);
    string_free(&mgr->workspace_path);
    free(mgr);
}

Session* session_manager_get(SessionManager* mgr, const char* key) {
    String key_str = string_new(key);
    for (size_t i = 0; i < mgr->count; i++) {
        if (string_equals(&mgr->sessions[i]->key, &key_str)) {
            string_free(&key_str);
            return mgr->sessions[i];
        }
    }
    string_free(&key_str);
    return NULL;
}

Session* session_manager_create(SessionManager* mgr, const char* key) {
    if (mgr->count >= mgr->capacity) {
        mgr->capacity *= 2;
        mgr->sessions = realloc(mgr->sessions, mgr->capacity * sizeof(Session*));
    }
    Session* session = malloc(sizeof(Session));
    session->key = string_new(key);
    session->messages = dynamic_array_new(sizeof(Message*));
    session->created_at = time(NULL);
    session->updated_at = time(NULL);
    session->last_consolidated = 0;
    mgr->sessions[mgr->count++] = session;
    return session;
}

Error session_manager_save(SessionManager* mgr, Session* session) {
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/sessions/%s.jsonl", mgr->workspace_path.data, session->key.data);
    FILE* f = fopen(filepath, "a");
    if (!f) return error_new(ERR_FILE, "Cannot open session file");
    
    // For simplicity, append new messages since last_consolidated
    for (size_t i = session->last_consolidated; i < session->messages.count; i++) {
        Message* msg = *(Message**)dynamic_array_get(&session->messages, i);
        
        // Skip saving tool calls/results to persistent session file to keep history clean
        // We only save User and Assistant messages (with content)
        if (msg->role == ROLE_TOOL) continue;
        if (msg->role == ROLE_ASSISTANT && msg->tool_calls_count > 0 && msg->content.len == 0) continue;

        // Simple JSON-like output
        fprintf(f, "{\"role\":\"%s\",\"content\":\"%s\",\"timestamp\":\"%s\"}\n",
                msg->role == ROLE_USER ? "user" : "assistant",
                msg->content.data, msg->timestamp.data);
    }
    fclose(f);
    session->last_consolidated = session->messages.count;
    return error_new(ERR_NONE, "");
}

#include "../vendor/cJSON/cJSON.h"

Error session_manager_load(SessionManager* mgr, const char* key, Session** session_out) {
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/sessions/%s.jsonl", mgr->workspace_path.data, key);
    FILE* f = fopen(filepath, "r");
    if (!f) {
        *session_out = session_manager_create(mgr, key);
        return error_new(ERR_NONE, "");
    }
    
    Session* session = session_manager_create(mgr, key);
    char line[4096]; // Increased buffer for longer messages
    while (fgets(line, sizeof(line), f)) {
        // Skip empty lines
        if (strlen(line) < 2) continue;
        
        cJSON* json = cJSON_Parse(line);
        if (!json) continue;
        
        cJSON* role_item = cJSON_GetObjectItem(json, "role");
        cJSON* content_item = cJSON_GetObjectItem(json, "content");
        
        if (cJSON_IsString(role_item) && cJSON_IsString(content_item)) {
            char* role_str = role_item->valuestring;
            char* content_str = content_item->valuestring;
            
            // Skip empty content to clean up bad history
            if (strlen(content_str) == 0) {
                cJSON_Delete(json);
                continue;
            }
            
            MessageRole role = ROLE_USER;
            if (strcmp(role_str, "assistant") == 0) role = ROLE_ASSISTANT;
            else if (strcmp(role_str, "tool") == 0) role = ROLE_TOOL;
            
            Message* msg = message_new(role, content_str);
            
            // Restore timestamp if available
            cJSON* ts_item = cJSON_GetObjectItem(json, "timestamp");
            if (cJSON_IsString(ts_item)) {
                string_free(&msg->timestamp);
                msg->timestamp = string_new(ts_item->valuestring);
            }
            
            session_add_message(session, msg);
        }
        cJSON_Delete(json);
    }
    fclose(f);
    
    // Set last_consolidated to current count so we don't re-save loaded messages
    session->last_consolidated = session->messages.count;
    
    *session_out = session;
    return error_new(ERR_NONE, "");
}

void session_add_message(Session* session, Message* msg) {
    dynamic_array_add(&session->messages, &msg);
    session->updated_at = time(NULL);
}