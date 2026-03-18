#ifndef MEMORY_H
#define MEMORY_H

#include "../include/common.h"
#include "../include/message.h"
#include "../include/config.h"

typedef struct {
    String memory_md; // Long-term facts
    String history_md; // Chronological log
    size_t max_tokens; // Maximum tokens before consolidation
    size_t current_tokens; // Current estimated token count
    double consolidation_threshold; // Threshold ratio for consolidation
} Memory;

// Functions
Memory* memory_new();
Memory* memory_new_with_config(Config* config);
void memory_free(Memory* mem);
Error memory_load(Memory* mem, const char* workspace_path);
Error memory_save(Memory* mem, const char* workspace_path);
void memory_add_fact(Memory* mem, const char* fact);
void memory_add_history(Memory* mem, const char* entry);

// Token estimation and consolidation
size_t memory_estimate_tokens(Memory* mem);
int memory_needs_consolidation(Memory* mem);
Error memory_consolidate(Memory* mem, const char* workspace_path);

#endif // MEMORY_H