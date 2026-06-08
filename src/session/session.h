#ifndef SESSION_H
#define SESSION_H

#include "../include/common.h"
#include "../include/message.h"
#include <pthread.h>

#define SESSION_HASHTABLE_BUCKETS 64

/*
 * Lock ordering protocol (must be followed to prevent deadlocks):
 *   1. SessionManager.rwlock  (read or write)
 *   2. Session.mutex
 *
 * Never acquire in reverse order. If Session.mutex is held,
 * do not attempt to acquire SessionManager.rwlock.
 *
 * Lifetime: a Session lives in the manager's hash table for the entire
 * lifetime of the manager. session_ref() / session_unref() track external
 * references but do NOT free the session. Free happens in
 * session_manager_free(). This avoids the get/unref race where a concurrent
 * get() could return a pointer the unref path was about to free.
 */

typedef struct Session Session;

typedef struct SessionEntry {
    Session* session;
    struct SessionEntry* next;
} SessionEntry;

struct Session {
    String key;
    DynamicArray messages;
    time_t created_at;
    time_t updated_at;
    size_t last_consolidated;
    size_t last_saved_count;
    bool needs_full_save;
    pthread_mutex_t mutex;
    int ref_count;
};

typedef struct {
    SessionEntry* buckets[SESSION_HASHTABLE_BUCKETS];
    size_t count;
    String workspace_path;
    pthread_mutex_t mutex;
    pthread_rwlock_t rwlock;
} SessionManager;

SessionManager* session_manager_new(const char* workspace_path);
void session_manager_free(SessionManager* mgr);
Session* session_manager_get(SessionManager* mgr, const char* key);
Session* session_manager_create(SessionManager* mgr, const char* key);
Error session_manager_save(SessionManager* mgr, Session* session);
Error session_manager_load(SessionManager* mgr, const char* key, Session** session);
void session_add_message(Session* session, Message* msg);
void session_rollback_messages(Session* session, size_t to_count);
Session* session_ref(Session* session);
void session_unref(Session* session);

#endif
