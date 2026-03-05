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
        // Simple JSON-like output
        fprintf(f, "{\"role\":\"%s\",\"content\":\"%s\",\"timestamp\":\"%s\"}\n",
                msg->role == ROLE_USER ? "user" : msg->role == ROLE_ASSISTANT ? "assistant" : "tool",
                msg->content.data, msg->timestamp.data);
    }
    fclose(f);
    session->last_consolidated = session->messages.count;
    return error_new(ERR_NONE, "");
}

Error session_manager_load(SessionManager* mgr, const char* key, Session** session_out) {
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/sessions/%s.jsonl", mgr->workspace_path.data, key);
    FILE* f = fopen(filepath, "r");
    if (!f) {
        *session_out = session_manager_create(mgr, key);
        return error_new(ERR_NONE, "");
    }
    Session* session = session_manager_create(mgr, key);
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        // Simple parse, assume format
        // For full impl, need JSON parser
        Message* msg = message_new(ROLE_USER, ""); // Placeholder
        session_add_message(session, msg);
    }
    fclose(f);
    *session_out = session;
    return error_new(ERR_NONE, "");
}

void session_add_message(Session* session, Message* msg) {
    dynamic_array_add(&session->messages, &msg);
    session->updated_at = time(NULL);
}