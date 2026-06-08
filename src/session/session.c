#include "session.h"
#include "../include/common.h"
#include "../include/message.h"
#include "../include/logger.h"
#include "../vendor/cJSON/cJSON.h"
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <stdint.h>
#include <unistd.h>

static uint32_t session_key_hash_fast(const char* key) {
    uint32_t hash = 5381;
    int c;
    while ((c = (unsigned char)*key++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash % SESSION_HASHTABLE_BUCKETS;
}

static uint64_t session_key_hash(const char* key) {
    const uint64_t fnv_offset = 1469598103934665603ULL;
    const uint64_t fnv_prime = 1099511628211ULL;
    uint64_t hash = fnv_offset;
    for (size_t i = 0; key[i]; i++) {
        hash ^= (unsigned char) key[i];
        hash *= fnv_prime;
    }
    return hash;
}

static void session_channel_prefix(const char* key, char* out, size_t out_size) {
    const char* sep = strchr(key, ':');
    size_t raw_len = sep ? (size_t)(sep - key) : strlen(key);
    size_t n = 0;
    for (size_t i = 0; i < raw_len && n + 1 < out_size && n < 12; i++) {
        unsigned char c = (unsigned char) key[i];
        if (isalnum(c) || c == '_' || c == '-') {
            out[n++] = (char)tolower(c);
        } else {
            out[n++] = '_';
        }
    }
    if (n == 0) {
        const char* fallback = "session";
        while (*fallback && n + 1 < out_size && n < 12) {
            out[n++] = *fallback++;
        }
    }
    out[n] = '\0';
}

static bool build_session_file_path(SessionManager* mgr, const char* key, char* filepath, size_t filepath_size) {
    if (filepath_size < 64) return false;
    uint64_t hash = session_key_hash(key);
    char channel[16];
    session_channel_prefix(key, channel, sizeof(channel));
    char compact_name[32];
    snprintf(compact_name, sizeof(compact_name), "%s_%016llx", channel, (unsigned long long) hash);
    int written = snprintf(filepath, filepath_size, "%s/sessions/%s.jsonl", mgr->workspace_path.data, compact_name);
    if (written < 0 || (size_t)written >= filepath_size) {
        log_error("[Session] Session filepath truncated for key: %s", key);
        return false;
    }
    return true;
}

static void build_legacy_session_file_path(SessionManager* mgr, const char* key, char* filepath, size_t filepath_size) {
    char sanitized[512];
    size_t j = 0;
    for (size_t i = 0; key[i] && j + 4 < sizeof(sanitized); i++) {
        unsigned char c = (unsigned char) key[i];
        if (isalnum(c) || c == '_' || c == '-' || c == '.') {
            sanitized[j++] = (char) c;
        } else {
            snprintf(sanitized + j, sizeof(sanitized) - j, "_%02X", c);
            j += 3;
        }
    }
    sanitized[j] = '\0';
    snprintf(filepath, filepath_size, "%s/sessions/%s.jsonl", mgr->workspace_path.data, sanitized);
}

static Session* session_manager_create_unlocked(SessionManager* mgr, const char* key) {
    if (!mgr || !key) return NULL;
    Session* session = calloc(1, sizeof(Session));
    if (!session) return NULL;
    session->key = string_new(key);
    session->messages = dynamic_array_new(sizeof(Message*));
    session->created_at = time(NULL);
    session->updated_at = time(NULL);
    session->last_consolidated = 0;
    session->last_saved_count = 0;
    session->needs_full_save = true;
    session->ref_count = 1;
    pthread_mutex_init(&session->mutex, NULL);
    
    uint32_t bucket = session_key_hash_fast(key);
    SessionEntry* entry = malloc(sizeof(SessionEntry));
    if (!entry) {
        pthread_mutex_destroy(&session->mutex);
        dynamic_array_free(&session->messages);
        string_free(&session->key);
        free(session);
        return NULL;
    }
    entry->session = session;
    entry->next = mgr->buckets[bucket];
    mgr->buckets[bucket] = entry;
    mgr->count++;
    return session;
}

SessionManager* session_manager_new(const char* workspace_path) {
    SessionManager* mgr = malloc(sizeof(SessionManager));
    if (!mgr) return NULL;
    
    for (size_t i = 0; i < SESSION_HASHTABLE_BUCKETS; i++) {
        mgr->buckets[i] = NULL;
    }
    mgr->count = 0;
    mgr->workspace_path = string_new(workspace_path);
    pthread_mutex_init(&mgr->mutex, NULL);
    pthread_rwlock_init(&mgr->rwlock, NULL);
    
    char path[FILE_PATH_MAX];
    snprintf(path, sizeof(path), "%s/sessions", workspace_path);
    struct stat st;
    if (stat(path, &st) != 0) {
        char parent[512];
        snprintf(parent, sizeof(parent), "%s", workspace_path);
        if (stat(parent, &st) != 0) {
            mkdir(parent, 0755);
        }
        mkdir(path, 0755);
    }
    return mgr;
}

void session_manager_free(SessionManager* mgr) {
    if (!mgr) return;
    pthread_rwlock_wrlock(&mgr->rwlock);
    for (size_t i = 0; i < SESSION_HASHTABLE_BUCKETS; i++) {
        SessionEntry* entry = mgr->buckets[i];
        while (entry) {
            SessionEntry* next = entry->next;
            string_free(&entry->session->key);
            for (size_t j = 0; j < entry->session->messages.count; j++) {
                Message* msg = *(Message**)dynamic_array_get(&entry->session->messages, j);
                message_free(msg);
            }
            dynamic_array_free(&entry->session->messages);
            pthread_mutex_destroy(&entry->session->mutex);
            free(entry->session);
            free(entry);
            entry = next;
        }
    }
    pthread_rwlock_unlock(&mgr->rwlock);
    string_free(&mgr->workspace_path);
    pthread_mutex_destroy(&mgr->mutex);
    pthread_rwlock_destroy(&mgr->rwlock);
    free(mgr);
}

Session* session_manager_get(SessionManager* mgr, const char* key) {
    if (!mgr || !key) return NULL;
    /* rdlock is sufficient: session_unref no longer frees (sessions are
     * freed by session_manager_free), so a borrow is race-free. */
    pthread_rwlock_rdlock(&mgr->rwlock);
    uint32_t bucket = session_key_hash_fast(key);
    SessionEntry* entry = mgr->buckets[bucket];
    while (entry) {
        if (strcmp(entry->session->key.data, key) == 0) {
            Session* found = entry->session;
            pthread_rwlock_unlock(&mgr->rwlock);
            return found;
        }
        entry = entry->next;
    }
    pthread_rwlock_unlock(&mgr->rwlock);
    return NULL;
}

Session* session_manager_create(SessionManager* mgr, const char* key) {
    if (!mgr || !key) return NULL;
    pthread_rwlock_wrlock(&mgr->rwlock);
    Session* session = session_manager_create_unlocked(mgr, key);
    pthread_rwlock_unlock(&mgr->rwlock);
    return session;
}

static void serialize_message_to_fp(FILE* f, Message* msg) {
    cJSON* msg_obj = cJSON_CreateObject();
    if (msg->role == ROLE_USER) cJSON_AddStringToObject(msg_obj, "role", "user");
    else if (msg->role == ROLE_ASSISTANT) cJSON_AddStringToObject(msg_obj, "role", "assistant");
    else cJSON_AddStringToObject(msg_obj, "role", "tool");
    cJSON_AddStringToObject(msg_obj, "content", msg->content.data ? msg->content.data : "");
    cJSON_AddStringToObject(msg_obj, "timestamp", msg->timestamp.data);

    if (msg->role == ROLE_ASSISTANT && msg->tool_calls_count > 0) {
        cJSON* tc_array = cJSON_CreateArray();
        for (size_t j = 0; j < msg->tool_calls_count; j++) {
            cJSON* tc_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(tc_obj, "id", msg->tool_calls[j].id.data);
            cJSON_AddStringToObject(tc_obj, "name", msg->tool_calls[j].name.data);
            cJSON_AddStringToObject(tc_obj, "arguments", msg->tool_calls[j].arguments.data);
            cJSON_AddItemToArray(tc_array, tc_obj);
        }
        cJSON_AddItemToObject(msg_obj, "tool_calls", tc_array);
    }

    if (msg->role == ROLE_TOOL) {
        if (msg->tool_call_id.len > 0) {
            cJSON_AddStringToObject(msg_obj, "tool_call_id", msg->tool_call_id.data);
        }
        if (msg->name.len > 0) {
            cJSON_AddStringToObject(msg_obj, "name", msg->name.data);
        }
    }

    char* msg_json = cJSON_PrintUnformatted(msg_obj);
    if (msg_json) {
        fprintf(f, "%s\n", msg_json);
        free(msg_json);
    }
    cJSON_Delete(msg_obj);
}

static void serialize_metadata_to_fp(FILE* f, Session* session, size_t save_count) {
    cJSON* meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "_type", "metadata");
    cJSON_AddStringToObject(meta, "key", session->key.data);
    cJSON_AddNumberToObject(meta, "created_at", (double)session->created_at);
    cJSON_AddNumberToObject(meta, "updated_at", (double)session->updated_at);
    cJSON_AddNumberToObject(meta, "last_consolidated", (double)save_count);
    char* meta_json = cJSON_PrintUnformatted(meta);
    if (meta_json) {
        fprintf(f, "%s\n", meta_json);
        free(meta_json);
    }
    cJSON_Delete(meta);
}

static bool file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

Error session_manager_save(SessionManager* mgr, Session* session) {
    if (!mgr || !session) return error_new(ERR_INVALID_PARAM, "Invalid parameters");

    char filepath[FILE_PATH_MAX];
    if (!build_session_file_path(mgr, session->key.data, filepath, sizeof(filepath))) {
        return error_new(ERR_FILE, "Session filepath too long");
    }

    pthread_mutex_lock(&session->mutex);
    size_t total_count = session->messages.count;
    size_t saved_count = session->last_saved_count;
    bool needs_full = session->needs_full_save || !file_exists(filepath) || saved_count > total_count;

    if (needs_full) {
        pthread_mutex_unlock(&session->mutex);
        char tmp_filepath[FILE_PATH_MAX + 8];
        snprintf(tmp_filepath, sizeof(tmp_filepath), "%s.tmp", filepath);

        FILE* f = fopen(tmp_filepath, "w");
        if (!f) return error_new(ERR_FILE, "Cannot open session file for writing");

        pthread_mutex_lock(&session->mutex);
        serialize_metadata_to_fp(f, session, total_count);
        for (size_t i = 0; i < total_count; i++) {
            Message* msg = *(Message**)dynamic_array_get(&session->messages, i);
            serialize_message_to_fp(f, msg);
        }
        session->last_saved_count = total_count;
        session->last_consolidated = total_count;
        session->needs_full_save = false;
        pthread_mutex_unlock(&session->mutex);
        fclose(f);

        if (rename(tmp_filepath, filepath) != 0) {
            unlink(tmp_filepath);
            return error_new(ERR_FILE, "Failed to commit session file");
        }
    } else {
        FILE* f = fopen(filepath, "a");
        if (!f) {
            pthread_mutex_unlock(&session->mutex);
            return error_new(ERR_FILE, "Cannot open session file for appending");
        }

        for (size_t i = saved_count; i < total_count; i++) {
            Message* msg = *(Message**)dynamic_array_get(&session->messages, i);
            serialize_message_to_fp(f, msg);
        }
        session->last_saved_count = total_count;
        pthread_mutex_unlock(&session->mutex);
        fclose(f);
    }

    return error_new(ERR_NONE, "");
}

Error session_manager_load(SessionManager* mgr, const char* key, Session** session_out) {
    char filepath[FILE_PATH_MAX];
    if (!build_session_file_path(mgr, key, filepath, sizeof(filepath))) {
        return error_new(ERR_FILE, "Session filepath too long");
    }
    FILE* f = fopen(filepath, "r");
    if (!f) {
        char legacy_filepath[512];
        build_legacy_session_file_path(mgr, key, legacy_filepath, sizeof(legacy_filepath));
        f = fopen(legacy_filepath, "r");
        if (!f) {
            *session_out = session_manager_create(mgr, key);
            if (!*session_out) {
                return error_new(ERR_MEMORY, "Failed to create session");
            }
            return error_new(ERR_NONE, "");
        }
    }

    Session* session = session_manager_create(mgr, key);
    if (!session) {
        fclose(f);
        return error_new(ERR_MEMORY, "Failed to create session");
    }
    char line[4096];
    bool metadata_loaded = false;

    while (fgets(line, sizeof(line), f)) {
        // Skip empty lines
        if (strlen(line) < 2) continue;

        cJSON* json = cJSON_Parse(line);
        if (!json) continue;

        // Check for metadata line (first line)
        cJSON* type_item = cJSON_GetObjectItem(json, "_type");
        if (cJSON_IsString(type_item) && strcmp(type_item->valuestring, "metadata") == 0) {
            // Load metadata
            cJSON* lc_item = cJSON_GetObjectItem(json, "last_consolidated");
            if (cJSON_IsNumber(lc_item)) {
                session->last_consolidated = (size_t)lc_item->valuedouble;
            }
            cJSON* created_item = cJSON_GetObjectItem(json, "created_at");
            if (cJSON_IsNumber(created_item)) {
                session->created_at = (time_t)created_item->valuedouble;
            }
            cJSON* updated_item = cJSON_GetObjectItem(json, "updated_at");
            if (cJSON_IsNumber(updated_item)) {
                session->updated_at = (time_t)updated_item->valuedouble;
            }
            metadata_loaded = true;
            cJSON_Delete(json);
            continue;
        }

        // Regular message line
        cJSON* role_item = cJSON_GetObjectItem(json, "role");
        cJSON* content_item = cJSON_GetObjectItem(json, "content");

        if (cJSON_IsString(role_item) && (cJSON_IsString(content_item) || cJSON_IsNull(content_item))) {
            char* role_str = role_item->valuestring;
            const char* content_str = cJSON_IsString(content_item) ? content_item->valuestring : "";

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
            if (role == ROLE_ASSISTANT) {
                cJSON* tcs = cJSON_GetObjectItem(json, "tool_calls");
                if (cJSON_IsArray(tcs)) {
                    int tc_count = cJSON_GetArraySize(tcs);
                    for (int i = 0; i < tc_count; i++) {
                        cJSON* tc = cJSON_GetArrayItem(tcs, i);
                        cJSON* id_item = cJSON_GetObjectItem(tc, "id");
                        cJSON* name_item = cJSON_GetObjectItem(tc, "name");
                        cJSON* args_item = cJSON_GetObjectItem(tc, "arguments");
                        if (cJSON_IsString(id_item) && cJSON_IsString(name_item) && cJSON_IsString(args_item)) {
                            message_add_tool_call(msg, id_item->valuestring, name_item->valuestring, args_item->valuestring);
                        }
                    }
                }
            } else if (role == ROLE_TOOL) {
                cJSON* tcid_item = cJSON_GetObjectItem(json, "tool_call_id");
                if (cJSON_IsString(tcid_item)) {
                    string_free(&msg->tool_call_id);
                    msg->tool_call_id = string_new(tcid_item->valuestring);
                }
                cJSON* name_item = cJSON_GetObjectItem(json, "name");
                if (cJSON_IsString(name_item)) {
                    string_free(&msg->name);
                    msg->name = string_new(name_item->valuestring);
                }
            }

            session_add_message(session, msg);
        }
        cJSON_Delete(json);
    }
    fclose(f);

    // If no metadata was loaded, set last_consolidated to current count
    // (for backward compatibility with old session files)
    if (!metadata_loaded) {
        pthread_mutex_lock(&session->mutex);
        session->last_consolidated = session->messages.count;
        pthread_mutex_unlock(&session->mutex);
    }

    pthread_mutex_lock(&session->mutex);
    session->last_saved_count = session->messages.count;
    session->needs_full_save = false;
    pthread_mutex_unlock(&session->mutex);

    *session_out = session;
    return error_new(ERR_NONE, "");
}

void session_add_message(Session* session, Message* msg) {
    pthread_mutex_lock(&session->mutex);
    dynamic_array_add(&session->messages, &msg);
    session->updated_at = time(NULL);
    pthread_mutex_unlock(&session->mutex);
}

void session_rollback_messages(Session* session, size_t to_count) {
    if (!session) return;
    pthread_mutex_lock(&session->mutex);
    if (to_count >= session->messages.count) {
        pthread_mutex_unlock(&session->mutex);
        return;
    }
    for (size_t i = to_count; i < session->messages.count; i++) {
        Message* msg = *(Message**)dynamic_array_get(&session->messages, i);
        if (msg) message_free(msg);
    }
    session->messages.count = to_count;
    if (session->last_saved_count > to_count) {
        session->last_saved_count = to_count;
    }
    if (session->last_consolidated > to_count) {
        session->last_consolidated = to_count;
    }
    session->updated_at = time(NULL);
    pthread_mutex_unlock(&session->mutex);
}

Session* session_ref(Session* session) {
    if (!session) return NULL;
    pthread_mutex_lock(&session->mutex);
    session->ref_count++;
    pthread_mutex_unlock(&session->mutex);
    return session;
}

void session_unref(Session* session) {
    if (!session) return;
    pthread_mutex_lock(&session->mutex);
    /* Sessions are kept alive in the manager's hash table; we just decrement.
     * The actual free happens in session_manager_free. Callers that want
     * strict lifecycle control should call session_manager_remove (TODO). */
    session->ref_count--;
    pthread_mutex_unlock(&session->mutex);
}
