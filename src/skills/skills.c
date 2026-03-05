#include "../include/skills.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../include/common.h"

struct SkillsLoader {
    char* workspace;
    char* builtin_skills_dir;
};

SkillsLoader* skills_loader_create(const char* workspace) {
    SkillsLoader* loader = malloc(sizeof(SkillsLoader));
    if (!loader) return NULL;

    loader->workspace = strdup(workspace);
    // In real implementation, would set builtin skills directory
    loader->builtin_skills_dir = NULL;

    return loader;
}

void skills_loader_destroy(SkillsLoader* loader) {
    if (!loader) return;

    free(loader->workspace);
    free(loader->builtin_skills_dir);
    free(loader);
}

StringArray* skills_loader_list_skills(SkillsLoader* loader, bool filter_unavailable) {
    // Stub implementation - return empty array
    (void)loader;
    (void)filter_unavailable;
    StringArray* arr = malloc(sizeof(StringArray));
    if (!arr) return NULL;
    *arr = string_array_new();
    return arr;
}

char* skills_loader_load_skill(SkillsLoader* loader, const char* name) {
    // Stub implementation
    (void)loader;
    (void)name;
    return NULL;
}

char* skills_loader_load_skills_for_context(SkillsLoader* loader, StringArray* skill_names) {
    // Stub implementation
    (void)loader;
    (void)skill_names;
    return NULL;
}

char* skills_loader_build_skills_summary(SkillsLoader* loader) {
    // Stub implementation - return basic summary
    (void)loader;
    return strdup("<skills></skills>");
}

StringArray* skills_loader_get_always_skills(SkillsLoader* loader) {
    // Stub implementation - return empty array
    (void)loader;
    StringArray* arr = malloc(sizeof(StringArray));
    if (!arr) return NULL;
    *arr = string_array_new();
    return arr;
}