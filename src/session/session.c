#include "session.h"
#include "../include/common.h"
#include "../include/message.h"
#include "../vendor/cJSON/cJSON.h"
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <stdint.h>

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

static void build_session_file_path(SessionManager* mgr, const char* key, char* filepath, size_t filepath_size) {
    uint64_t hash = session_key_hash(key);
    char channel[16];
    session_channel_prefix(key, channel, sizeof(channel));
    char compact_name[32];
    snprintf(compact_name, sizeof(compact_name), "%s_%016llx", channel, (unsigned long long) hash);
    snprintf(filepath, filepath_size, "%s/sessions/%s.jsonl", mgr->workspace_path.data, compact_name);
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
    if (mgr->count >= mgr->capacity) {
        if (mgr->capacity > SIZE_MAX / 2) return NULL;
        size_t new_capacity = mgr->capacity * 2;
        if (new_capacity > SIZE_MAX / sizeof(Session*)) return NULL;
        Session** new_sessions = realloc(mgr->sessions, new_capacity * sizeof(Session*));
        if (!new_sessions) return NULL;
        mgr->sessions = new_sessions;
        mgr->capacity = new_capacity;
    }
    Session* session = calloc(1, sizeof(Session));
    if (!session) return NULL;
    session->key = string_new(key);
    session->messages = dynamic_array_new(sizeof(Message*));
    session->created_at = time(NULL);
    session->updated_at = time(NULL);
    session->last_consolidated = 0;
    pthread_mutex_init(&session->mutex, NULL);
    mgr->sessions[mgr->count++] = session;
    return session;
}

SessionManager* session_manager_new(const char* workspace_path) {
    SessionManager* mgr = malloc(sizeof(SessionManager));
    if (!mgr) return NULL;
    mgr->count = 0;
    mgr->capacity = 8;
    mgr->sessions = malloc(mgr->capacity * sizeof(Session*));
    if (!mgr->sessions) {
        free(mgr);
        return NULL;
    }
    mgr->workspace_path = string_new(workspace_path);
    pthread_mutex_init(&mgr->mutex, NULL);
    // Create sessions directory if not exists (including parent directories)
    char path[512];
    snprintf(path, sizeof(path), "%s/sessions", workspace_path);
    // Use system mkdir -p equivalent by trying to create with parents
    // First ensure parent directory exists (workspace_path should exist)
    // Then create sessions subdirectory
    struct stat st;
    if (stat(path, &st) != 0) {
        // Directory doesn't exist, try to create it
        // Try creating parent first if needed
        char parent[512];
        snprintf(parent, sizeof(parent), "%s", workspace_path);
        if (stat(parent, &st) != 0) {
            mkdir(parent, 0755);  // Create workspace if needed
        }
        mkdir(path, 0755);  // Create sessions subdirectory
    }
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
        pthread_mutex_destroy(&mgr->sessions[i]->mutex);
        free(mgr->sessions[i]);
    }
    free(mgr->sessions);
    string_free(&mgr->workspace_path);
    pthread_mutex_destroy(&mgr->mutex);
    free(mgr);
}

Session* session_manager_get(SessionManager* mgr, const char* key) {
    if (!mgr || !key) return NULL;
    String key_str = string_new(key);
    pthread_mutex_lock(&mgr->mutex);
    for (size_t i = 0; i < mgr->count; i++) {
        if (string_equals(&mgr->sessions[i]->key, &key_str)) {
            Session* found = mgr->sessions[i];
            pthread_mutex_unlock(&mgr->mutex);
            string_free(&key_str);
            return found;
        }
    }
    pthread_mutex_unlock(&mgr->mutex);
    string_free(&key_str);
    return NULL;
}

Session* session_manager_create(SessionManager* mgr, const char* key) {
    if (!mgr || !key) return NULL;
    pthread_mutex_lock(&mgr->mutex);
    Session* session = session_manager_create_unlocked(mgr, key);
    pthread_mutex_unlock(&mgr->mutex);
    return session;
}

Error session_manager_save(SessionManager* mgr, Session* session) {
    char filepath[512];
    build_session_file_path(mgr, session->key.data, filepath, sizeof(filepath));
    FILE* f = fopen(filepath, "w");
    if (!f) return error_new(ERR_FILE, "Cannot open session file");

    /* Write metadata line first */
    fprintf(f, "{\"_type\":\"metadata\",\"key\":\"%s\",\"created_at\":%ld,\"updated_at\":%ld,\"last_consolidated\":%zu}\n",
            session->key.data, (long)session->created_at, (long)session->updated_at, session->last_consolidated);

    pthread_mutex_lock(&session->mutex);
    for (size_t i = 0; i < session->messages.count; i++) {
        Message* msg = *(Message**)dynamic_array_get(&session->messages, i);

        if (msg->role == ROLE_TOOL) continue;
        if (msg->role == ROLE_ASSISTANT && msg->content.len == 0) continue;

        cJSON* msg_obj = cJSON_CreateObject();
        if (msg->role == ROLE_USER) cJSON_AddStringToObject(msg_obj, "role", "user");
        else if (msg->role == ROLE_ASSISTANT) cJSON_AddStringToObject(msg_obj, "role", "assistant");
        else cJSON_AddStringToObject(msg_obj, "role", "tool");
        if (msg->content.len > 0) {
            cJSON_AddStringToObject(msg_obj, "content", msg->content.data);
        }
        cJSON_AddStringToObject(msg_obj, "timestamp", msg->timestamp.data);

        char* msg_json = cJSON_PrintUnformatted(msg_obj);
        if (msg_json) {
            fprintf(f, "%s\n", msg_json);
            free(msg_json);
        }
        cJSON_Delete(msg_obj);
    }
    pthread_mutex_unlock(&session->mutex);
    fclose(f);
    return error_new(ERR_NONE, "");
}

#include "../vendor/cJSON/cJSON.h"

Error session_manager_load(SessionManager* mgr, const char* key, Session** session_out) {
    char filepath[512];
    build_session_file_path(mgr, key, filepath, sizeof(filepath));
    FILE* f = fopen(filepath, "r");
    if (!f) {
        char legacy_filepath[512];
        build_legacy_session_file_path(mgr, key, legacy_filepath, sizeof(legacy_filepath));
        f = fopen(legacy_filepath, "r");
        if (!f) {
            *session_out = session_manager_create(mgr, key);
            return error_new(ERR_NONE, "");
        }
    }

    Session* session = session_manager_create(mgr, key);
    char line[4096]; // Increased buffer for longer messages
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
        session->last_consolidated = session->messages.count;
    }

    *session_out = session;
    return error_new(ERR_NONE, "");
}

void session_add_message(Session* session, Message* msg) {
    pthread_mutex_lock(&session->mutex);
    dynamic_array_add(&session->messages, &msg);
    session->updated_at = time(NULL);
    pthread_mutex_unlock(&session->mutex);
}
