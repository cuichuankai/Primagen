#ifndef SESSION_H
#define SESSION_H

#include "../include/common.h"
#include "../include/message.h"
#include <pthread.h>

typedef struct {
    String key; // "channel:chat_id"
    DynamicArray messages; // Message*
    time_t created_at;
    time_t updated_at;
    size_t last_consolidated;
    pthread_mutex_t mutex;
} Session;

typedef struct {
    Session** sessions;
    size_t count;
    size_t capacity;
    String workspace_path;
} SessionManager;

// Functions
SessionManager* session_manager_new(const char* workspace_path);
void session_manager_free(SessionManager* mgr);
Session* session_manager_get(SessionManager* mgr, const char* key);
Session* session_manager_create(SessionManager* mgr, const char* key);
Error session_manager_save(SessionManager* mgr, Session* session);
Error session_manager_load(SessionManager* mgr, const char* key, Session** session);
void session_add_message(Session* session, Message* msg);

#endif // SESSION_H
