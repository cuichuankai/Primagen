#include "memory.h"
#include "../include/common.h"

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
    
    // Load MEMORY.md
    snprintf(path, sizeof(path), "%s/MEMORY.md", workspace_path);
    f = fopen(path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        fseek(f, 0, SEEK_SET);
        char* buf = malloc(size + 1);
        fread(buf, 1, size, f);
        buf[size] = '\0';
        string_free(&mem->memory_md);
        mem->memory_md = string_new(buf);
        free(buf);
        fclose(f);
    }
    
    // Load HISTORY.md
    snprintf(path, sizeof(path), "%s/HISTORY.md", workspace_path);
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
    
    // Save MEMORY.md
    snprintf(path, sizeof(path), "%s/MEMORY.md", workspace_path);
    f = fopen(path, "w");
    if (!f) return error_new(ERR_FILE, "Cannot save MEMORY.md");
    fwrite(mem->memory_md.data, 1, mem->memory_md.len, f);
    fclose(f);
    
    // Save HISTORY.md
    snprintf(path, sizeof(path), "%s/HISTORY.md", workspace_path);
    f = fopen(path, "w");
    if (!f) return error_new(ERR_FILE, "Cannot save HISTORY.md");
    fwrite(mem->history_md.data, 1, mem->history_md.len, f);
    fclose(f);
    
    return error_new(ERR_NONE, "");
}

void memory_add_fact(Memory* mem, const char* fact) {
    // Append to memory_md
    char* new_data = malloc(mem->memory_md.len + strlen(fact) + 2);
    strcpy(new_data, mem->memory_md.data);
    strcat(new_data, fact);
    strcat(new_data, "\n");
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