#ifndef MEMORY_H
#define MEMORY_H

#include "../include/common.h"

typedef struct {
    String memory_md; // Long-term facts
    String history_md; // Chronological log
} Memory;

// Functions
Memory* memory_new();
void memory_free(Memory* mem);
Error memory_load(Memory* mem, const char* workspace_path);
Error memory_save(Memory* mem, const char* workspace_path);
void memory_add_fact(Memory* mem, const char* fact);
void memory_add_history(Memory* mem, const char* entry);

#endif // MEMORY_H