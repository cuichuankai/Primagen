#include "memory.h"
#include "../include/common.h"
#include <sys/stat.h>

Memory* memory_new() {
    Memory* mem = malloc(sizeof(Memory));
    if (!mem) return NULL;
    mem->memory_md = string_new("");
    mem->history_md = string_new("");
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
        fread(buf, 1, size, f);
        buf[size] = '\0';
        string_free(&mem->history_md);
        mem->history_md = string_new(buf);
        free(buf);
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
    strcpy(new_data, mem->history_md.data);
    strcat(new_data, entry);
    strcat(new_data, "\n");
    string_free(&mem->history_md);
    mem->history_md = string_new(new_data);
    free(new_data);
}