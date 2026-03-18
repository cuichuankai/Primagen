#include "memory.h"
#include "../include/common.h"
#include "../include/utils.h"
#include <sys/stat.h>

#define DEFAULT_MAX_TOKENS 4000  // Maximum tokens before consolidation
#define CONSOLIDATION_THRESHOLD 0.8  // Consolidate when 80% full

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
    return mem;
}

void memory_free(Memory* mem) {
    if (!mem) return;
    string_free(&mem->memory_md);
    string_free(&mem->history_md);
    free(mem);
}

Error memory_load(Memory* mem, const char* workspace_path) {
    char path[512];
    FILE* f;
    
    // Ensure memory directory exists
    snprintf(path, sizeof(path), "%s/memory", workspace_path);
    mkdir(path, 0755);

    // Load MEMORY.md
    snprintf(path, sizeof(path), "%s/memory/MEMORY.md", workspace_path);
    f = fopen(path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        fseek(f, 0, SEEK_SET);
        char* buf = malloc(size + 1);
        if (buf) {
            fread(buf, 1, size, f);
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
            mem->memory_md = string_new(default_mem);
        }
    }
    
    // Load HISTORY.md
    snprintf(path, sizeof(path), "%s/memory/HISTORY.md", workspace_path);
    f = fopen(path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        fseek(f, 0, SEEK_SET);
        char* buf = malloc(size + 1);
        if (buf) {
            fread(buf, 1, size, f);
            buf[size] = '\0';
            string_free(&mem->history_md);
            mem->history_md = string_new(buf);
            free(buf);
        }
        fclose(f);
    }
    
    return error_new(ERR_NONE, "");
}

Error memory_save(Memory* mem, const char* workspace_path) {
    char path[512];
    FILE* f;
    
    // Ensure memory directory exists
    snprintf(path, sizeof(path), "%s/memory", workspace_path);
    mkdir(path, 0755);

    // Save MEMORY.md
    snprintf(path, sizeof(path), "%s/memory/MEMORY.md", workspace_path);
    f = fopen(path, "w");
    if (!f) return error_new(ERR_FILE, "Cannot save MEMORY.md");
    fwrite(mem->memory_md.data, 1, mem->memory_md.len, f);
    fclose(f);
    
    // Save HISTORY.md
    snprintf(path, sizeof(path), "%s/memory/HISTORY.md", workspace_path);
    f = fopen(path, "w");
    if (!f) return error_new(ERR_FILE, "Cannot save HISTORY.md");
    fwrite(mem->history_md.data, 1, mem->history_md.len, f);
    fclose(f);
    
    return error_new(ERR_NONE, "");
}

void memory_add_fact(Memory* mem, const char* fact) {
    const char* section = "## Important Notes";
    char* pos = strstr(mem->memory_md.data, section);
    
    // Default to appending at the end
    size_t insert_idx = mem->memory_md.len;
    
    if (pos) {
        // Move pos to end of section header
        pos += strlen(section);
        
        // Find next section
        char* next = strstr(pos, "\n## ");
        if (next) {
            insert_idx = next - mem->memory_md.data;
        }
    }
    
    // Calculate lengths
    size_t fact_len = strlen(fact);
    // Determine padding needed before fact
    bool need_newline = (insert_idx > 0 && mem->memory_md.data[insert_idx-1] != '\n');
    
    // Total new length: original + padding + "\n- " + fact + "\n" + null
    // Padding: 1 char if needed
    // "\n- ": 3 chars (or "- " if already newlined)
    // "\n": 1 char (trailing)
    // Safety margin: 10
    size_t new_total_len = mem->memory_md.len + fact_len + 20;
    
    char* new_data = malloc(new_total_len);
    if (!new_data) return;
    
    // Copy prefix
    if (insert_idx > 0) {
        memcpy(new_data, mem->memory_md.data, insert_idx);
    }
    new_data[insert_idx] = '\0';
    
    // Append fact
    if (need_newline) strcat(new_data, "\n");
    strcat(new_data, "- ");
    strcat(new_data, fact);
    strcat(new_data, "\n");
    
    // Append suffix (rest of the file)
    if (insert_idx < mem->memory_md.len) {
        strcat(new_data, mem->memory_md.data + insert_idx);
    }
    
    string_free(&mem->memory_md);
    mem->memory_md = string_new(new_data);
    free(new_data);
}

void memory_add_history(Memory* mem, const char* entry) {
    // Append to history_md
    char* new_data = malloc(mem->history_md.len + strlen(entry) + 2);
    if (!new_data) return;  // Memory allocation failed, silently skip
    strcpy(new_data, mem->history_md.data);
    strcat(new_data, entry);
    strcat(new_data, "\n");
    string_free(&mem->history_md);
    mem->history_md = string_new(new_data);
    free(new_data);

    // Update token count
    mem->current_tokens = memory_estimate_tokens(mem);
}

/**
 * Estimate tokens in memory content
 */
size_t memory_estimate_tokens(Memory* mem) {
    if (!mem) return 0;
    return estimate_tokens(mem->memory_md.data) + estimate_tokens(mem->history_md.data);
}

/**
 * Check if memory needs consolidation
 */
int memory_needs_consolidation(Memory* mem) {
    if (!mem) return 0;

    size_t current = memory_estimate_tokens(mem);
    mem->current_tokens = current;

    return current > (mem->max_tokens * mem->consolidation_threshold);
}

/**
 * Consolidate memory by summarizing old history
 * This is a simple implementation that truncates old history when threshold is exceeded
 */
Error memory_consolidate(Memory* mem, const char* workspace_path) {
    if (!mem || !workspace_path) return error_new(ERR_INVALID_PARAM, "Invalid arguments");

    // Check if consolidation is needed
    if (!memory_needs_consolidation(mem)) {
        return error_new(ERR_NONE, "");
    }

    // Simple consolidation: keep only recent history
    // Find the last N entries (by counting newlines)
    const char* data = mem->history_md.data;
    size_t len = mem->history_md.len;

    // Keep last 5 entries (count newlines)
    int entries_to_keep = 5;
    const char* cut_pos = data + len;
    int newline_count = 0;

    while (cut_pos > data && newline_count < entries_to_keep) {
        cut_pos--;
        if (*cut_pos == '\n') {
            newline_count++;
        }
    }

    // Create consolidated history
    if (cut_pos > data) {
        // Add consolidation marker
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

    // Save consolidated memory
    return memory_save(mem, workspace_path);
}