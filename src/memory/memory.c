#include "memory.h"
#include "../include/common.h"
#include "../include/utils.h"
#include "../include/logger.h"
#include <sys/stat.h>
#include <ctype.h>

#define DEFAULT_MAX_TOKENS 4000  // Maximum tokens before consolidation
#define CONSOLIDATION_THRESHOLD 0.8  // Consolidate when 80% full

static void append_history_buffer_unlocked(Memory* mem, const char* entry) {
    char* new_data = malloc(mem->history_md.len + strlen(entry) + 2);
    if (!new_data) return;
    strcpy(new_data, mem->history_md.data);
    strcat(new_data, entry);
    strcat(new_data, "\n");
    string_free(&mem->history_md);
    mem->history_md = string_new(new_data);
    free(new_data);
    mem->current_tokens = estimate_tokens(mem->memory_md.data) + estimate_tokens(mem->history_md.data);
}

static bool fact_exists_unlocked(Memory* mem, const char* fact) {
    size_t need = strlen(fact) + 4;
    char* needle = malloc(need);
    if (!needle) return false;
    snprintf(needle, need, "- %s", fact);
    bool exists = strstr(mem->memory_md.data, needle) != NULL;
    free(needle);
    return exists;
}

static void add_fact_unlocked(Memory* mem, const char* fact) {
    if (!mem || !fact || fact[0] == '\0') return;
    if (fact_exists_unlocked(mem, fact)) return;
    const char* section = "## Important Notes";
    char* pos = strstr(mem->memory_md.data, section);
    size_t insert_idx = mem->memory_md.len;
    if (pos) {
        pos += strlen(section);
        char* next = strstr(pos, "\n## ");
        if (next) {
            insert_idx = (size_t)(next - mem->memory_md.data);
        }
    }
    size_t fact_len = strlen(fact);
    bool need_newline = (insert_idx > 0 && mem->memory_md.data[insert_idx - 1] != '\n');
    size_t new_total_len = mem->memory_md.len + fact_len + 64;
    char* new_data = malloc(new_total_len);
    if (!new_data) return;
    if (insert_idx > 0) {
        memcpy(new_data, mem->memory_md.data, insert_idx);
    }
    new_data[insert_idx] = '\0';
    if (need_newline) strcat(new_data, "\n");
    strcat(new_data, "- ");
    strcat(new_data, fact);
    strcat(new_data, "\n");
    if (insert_idx < mem->memory_md.len) {
        strcat(new_data, mem->memory_md.data + insert_idx);
    }
    string_free(&mem->memory_md);
    mem->memory_md = string_new(new_data);
    free(new_data);
}

Memory* memory_new() {
    return memory_new_with_config(NULL);
}

Memory* memory_new_with_config(Config* config) {
    Memory* mem = malloc(sizeof(Memory));
    if (!mem) return NULL;
    mem->memory_md = string_new("");
    mem->history_md = string_new("");
    // Get settings from config, use defaults if not set
    mem->max_tokens = config && config->agent.memory_max_tokens > 0
                      ? (size_t)config->agent.memory_max_tokens : DEFAULT_MAX_TOKENS;
    mem->current_tokens = 0;
    mem->consolidation_threshold = config && config->agent.memory_consolidation_threshold > 0
                                   ? config->agent.memory_consolidation_threshold : CONSOLIDATION_THRESHOLD;
    pthread_mutex_init(&mem->mutex, NULL);
    return mem;
}

void memory_free(Memory* mem) {
    if (!mem) return;
    pthread_mutex_destroy(&mem->mutex);
    string_free(&mem->memory_md);
    string_free(&mem->history_md);
    free(mem);
}

Error memory_load(Memory* mem, const char* workspace_path) {
    char path[FILE_PATH_MAX];
    FILE* f;
    
    if (!mem || !workspace_path) return error_new(ERR_INVALID_PARAM, "Invalid arguments");
    pthread_mutex_lock(&mem->mutex);

    snprintf(path, sizeof(path), "%s/memory", workspace_path);
    mkdir(path, 0755);

    // Load MEMORY.md
    snprintf(path, sizeof(path), "%s/memory/MEMORY.md", workspace_path);
    f = fopen(path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsize < 0) fsize = 0;
        size_t size = (size_t)fsize;
        char* buf = malloc(size + 1);
        if (buf) {
            if (size > 0) fread(buf, 1, size, f);
            buf[size] = '\0';
            string_free(&mem->memory_md);
            mem->memory_md = string_new(buf);
            free(buf);
        }
        fclose(f);
    } else {
        // Create with default template if not exists
        const char* default_mem = "# Long-term Memory\n\nThis file stores important information that should persist across sessions.\n\n## User Information\n\n(Important facts about the user)\n\n## Preferences\n\n(User preferences learned over time)\n\n## Important Notes\n\n(Things to remember)\n";
        f = fopen(path, "w");
        if (f) {
            fputs(default_mem, f);
            fclose(f);
            string_free(&mem->memory_md);
            mem->memory_md = string_new(default_mem);
        }
    }
    
    // Load HISTORY.md
    snprintf(path, sizeof(path), "%s/memory/HISTORY.md", workspace_path);
    f = fopen(path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsize < 0) fsize = 0;
        size_t size = (size_t)fsize;
        char* buf = malloc(size + 1);
        if (buf) {
            if (size > 0) fread(buf, 1, size, f);
            buf[size] = '\0';
            string_free(&mem->history_md);
            mem->history_md = string_new(buf);
            free(buf);
        }
        fclose(f);
    } else {
        string_free(&mem->history_md);
        mem->history_md = string_new("");
    }
    mem->current_tokens = estimate_tokens(mem->memory_md.data) + estimate_tokens(mem->history_md.data);
    pthread_mutex_unlock(&mem->mutex);
    return error_new(ERR_NONE, "");
}

Error memory_save(Memory* mem, const char* workspace_path) {
    if (!mem || !workspace_path) return error_new(ERR_INVALID_PARAM, "Invalid arguments");
    pthread_mutex_lock(&mem->mutex);
    char path[FILE_PATH_MAX];
    FILE* f;
    
    // Ensure memory directory exists
    snprintf(path, sizeof(path), "%s/memory", workspace_path);
    mkdir(path, 0755);

    // Save MEMORY.md
    snprintf(path, sizeof(path), "%s/memory/MEMORY.md", workspace_path);
    f = fopen(path, "w");
    if (!f) {
        pthread_mutex_unlock(&mem->mutex);
        return error_new(ERR_FILE, "Cannot save MEMORY.md");
    }
    fwrite(mem->memory_md.data, 1, mem->memory_md.len, f);
    fclose(f);
    
    // Save HISTORY.md
    snprintf(path, sizeof(path), "%s/memory/HISTORY.md", workspace_path);
    f = fopen(path, "w");
    if (!f) {
        pthread_mutex_unlock(&mem->mutex);
        return error_new(ERR_FILE, "Cannot save HISTORY.md");
    }
    fwrite(mem->history_md.data, 1, mem->history_md.len, f);
    fclose(f);
    pthread_mutex_unlock(&mem->mutex);
    return error_new(ERR_NONE, "");
}

void memory_add_fact(Memory* mem, const char* fact) {
    if (!mem || !fact || fact[0] == '\0') return;
    pthread_mutex_lock(&mem->mutex);
    add_fact_unlocked(mem, fact);
    mem->current_tokens = estimate_tokens(mem->memory_md.data) + estimate_tokens(mem->history_md.data);
    pthread_mutex_unlock(&mem->mutex);
}

void memory_add_history(Memory* mem, const char* entry) {
    if (!mem || !entry) return;
    pthread_mutex_lock(&mem->mutex);
    append_history_buffer_unlocked(mem, entry);
    pthread_mutex_unlock(&mem->mutex);
}

Error memory_append_history(Memory* mem, const char* workspace_path, const char* entry) {
    if (!mem || !workspace_path || !entry) {
        return error_new(ERR_INVALID_PARAM, "Invalid arguments");
    }

    pthread_mutex_lock(&mem->mutex);
    char path[FILE_PATH_MAX];
    snprintf(path, sizeof(path), "%s/memory", workspace_path);
    mkdir(path, 0755);

    snprintf(path, sizeof(path), "%s/memory/HISTORY.md", workspace_path);
    FILE* f = fopen(path, "a");
    if (!f) {
        pthread_mutex_unlock(&mem->mutex);
        return error_new(ERR_FILE, "Cannot append HISTORY.md");
    }

    fputs(entry, f);
    fputs("\n", f);
    fclose(f);

    append_history_buffer_unlocked(mem, entry);
    pthread_mutex_unlock(&mem->mutex);
    return error_new(ERR_NONE, "");
}

Error memory_set_facts(Memory* mem, const char* memory_update) {
    if (!mem || !memory_update) return error_new(ERR_INVALID_PARAM, "Invalid arguments");
    pthread_mutex_lock(&mem->mutex);

    size_t old_len = mem->memory_md.len;
    size_t new_len = strlen(memory_update);

    if (old_len > 100 && new_len < old_len / 3) {
        log_warn("[Memory] Rejecting memory_update: new content (%zu bytes) is less than 1/3 of existing content (%zu bytes). This likely means existing facts were not included. Use the 'content' parameter to add facts safely.",
                 new_len, old_len);
        pthread_mutex_unlock(&mem->mutex);
        return error_new(ERR_INVALID_PARAM, "Rejected: new content is much shorter than existing memory. You MUST include ALL existing facts in memory_update. Use 'content' parameter to safely add new facts without replacing existing ones.");
    }

    string_free(&mem->memory_md);
    mem->memory_md = string_new(memory_update);
    mem->current_tokens = estimate_tokens(mem->memory_md.data) + estimate_tokens(mem->history_md.data);
    pthread_mutex_unlock(&mem->mutex);
    return error_new(ERR_NONE, "");
}

/**
 * Estimate tokens in memory content
 */
size_t memory_estimate_tokens(Memory* mem) {
    if (!mem) return 0;
    pthread_mutex_lock(&mem->mutex);
    size_t t = estimate_tokens(mem->memory_md.data) + estimate_tokens(mem->history_md.data);
    pthread_mutex_unlock(&mem->mutex);
    return t;
}

/**
 * Check if memory needs consolidation
 */
int memory_needs_consolidation(Memory* mem) {
    if (!mem) return 0;
    pthread_mutex_lock(&mem->mutex);
    size_t current = estimate_tokens(mem->memory_md.data) + estimate_tokens(mem->history_md.data);
    mem->current_tokens = current;
    int needed = current > (mem->max_tokens * mem->consolidation_threshold);
    pthread_mutex_unlock(&mem->mutex);
    return needed;
}

/**
 * Consolidate memory by summarizing old history
 * This is a simple implementation that truncates old history when threshold is exceeded
 */
Error memory_consolidate(Memory* mem, const char* workspace_path) {
    if (!mem || !workspace_path) return error_new(ERR_INVALID_PARAM, "Invalid arguments");
    pthread_mutex_lock(&mem->mutex);
    size_t current = estimate_tokens(mem->memory_md.data) + estimate_tokens(mem->history_md.data);
    mem->current_tokens = current;
    if (!(current > (mem->max_tokens * mem->consolidation_threshold))) {
        pthread_mutex_unlock(&mem->mutex);
        return error_new(ERR_NONE, "");
    }

    const char* data = mem->history_md.data;
    size_t len = mem->history_md.len;

    int entries_to_keep = 5;
    const char* cut_pos = data + len;
    int newline_count = 0;

    while (cut_pos > data && newline_count < entries_to_keep) {
        cut_pos--;
        if (*cut_pos == '\n') {
            newline_count++;
        }
    }

    size_t old_len = (cut_pos > data) ? (size_t)(cut_pos - data) : 0;
    char* old_part = NULL;
    if (old_len > 0) {
        old_part = malloc(old_len + 1);
        if (old_part) {
            memcpy(old_part, data, old_len);
            old_part[old_len] = '\0';
        }
    }

    if (cut_pos > data) {
        const char* marker = "\n\n[Previous history consolidated to save space]\n";
        size_t marker_len = strlen(marker);
        size_t remaining_len = len - (cut_pos - data);

        char* new_history = malloc(marker_len + remaining_len + 1);
        if (new_history) {
            strcpy(new_history, marker);
            strcat(new_history, cut_pos);

            string_free(&mem->history_md);
            mem->history_md = string_new(new_history);
            free(new_history);
        }
    }
    if (old_part) {
            char* saveptr = NULL;
            char* line = strtok_r(old_part, "\n", &saveptr);
            int added = 0;
            while (line && added < 5) {
                while (*line && isspace((unsigned char)*line)) line++;
                if (*line != '\0' && strstr(line, "consolidated to save space") == NULL) {
                    const char* fact = line;
                    if (line[0] == '[') {
                        char* after = strchr(line, ']');
                        if (after && after[1] == ' ') fact = after + 2;
                    }
                    if (*fact != '\0' && !fact_exists_unlocked(mem, fact)) {
                        add_fact_unlocked(mem, fact);
                        added++;
                    }
                }
                line = strtok_r(NULL, "\n", &saveptr);
            }
        free(old_part);
    }
    mem->current_tokens = estimate_tokens(mem->memory_md.data) + estimate_tokens(mem->history_md.data);
    char mem_copy[FILE_PATH_MAX];
    snprintf(mem_copy, sizeof(mem_copy), "%s/memory", workspace_path);
    mkdir(mem_copy, 0755);
    char memory_file[FILE_PATH_MAX];
    snprintf(memory_file, sizeof(memory_file), "%s/memory/MEMORY.md", workspace_path);
    FILE* f1 = fopen(memory_file, "w");
    if (!f1) {
        pthread_mutex_unlock(&mem->mutex);
        return error_new(ERR_FILE, "Cannot save MEMORY.md");
    }
    fwrite(mem->memory_md.data, 1, mem->memory_md.len, f1);
    fclose(f1);

    char history_file[FILE_PATH_MAX];
    snprintf(history_file, sizeof(history_file), "%s/memory/HISTORY.md", workspace_path);
    FILE* f2 = fopen(history_file, "w");
    if (!f2) {
        pthread_mutex_unlock(&mem->mutex);
        return error_new(ERR_FILE, "Cannot save HISTORY.md");
    }
    fwrite(mem->history_md.data, 1, mem->history_md.len, f2);
    fclose(f2);
    pthread_mutex_unlock(&mem->mutex);
    return error_new(ERR_NONE, "");
}
