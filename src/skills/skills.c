#include "../include/skills.h"
#include "../include/logger.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../include/common.h"
#include "../vendor/cJSON/cJSON.h"

struct SkillsLoader {
    char* workspace;
    char* builtin_skills_dir;
};

// Helper: Check if file exists
static bool file_exists(const char* path) {
    return access(path, F_OK) != -1;
}

// Helper: Read entire file
static char* read_file(const char* path) {
    FILE* fp = fopen(path, "r");
    if (!fp) return NULL;
    
    fseek(fp, 0, SEEK_END);
    long length = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (length < 0) { fclose(fp); return NULL; }
    
    char* buffer = malloc(length + 1);
    if (!buffer) { fclose(fp); return NULL; }
    
    fread(buffer, 1, length, fp);
    buffer[length] = '\0';
    fclose(fp);
    return buffer;
}

// Helper: Check if command exists in PATH
static bool command_exists(const char* cmd) {
    char* path_env = getenv("PATH");
    if (!path_env) return false;
    
    char* path = strdup(path_env);
    char* token = strtok(path, ":");
    char full_path[1024];
    
    while (token) {
        snprintf(full_path, sizeof(full_path), "%s/%s", token, cmd);
        if (access(full_path, X_OK) == 0) {
            free(path);
            return true;
        }
        token = strtok(NULL, ":");
    }
    
    free(path);
    return false;
}

// Helper: Parse simple YAML frontmatter (key: value)
static char* get_frontmatter_value(const char* content, const char* key) {
    if (!content || strncmp(content, "---\n", 4) != 0) return NULL;
    
    const char* end_fm = strstr(content + 4, "\n---\n");
    if (!end_fm) return NULL;
    
    char* value = NULL;
    size_t key_len = strlen(key);
    
    // Search line by line within frontmatter
    const char* p = content + 4;
    while (p < end_fm) {
        const char* line_end = strchr(p, '\n');
        if (!line_end || line_end > end_fm) line_end = end_fm;
        
        if (strncmp(p, key, key_len) == 0 && p[key_len] == ':') {
            const char* v_start = p + key_len + 1;
            while (v_start < line_end && (*v_start == ' ' || *v_start == '\t')) v_start++;
            
            size_t v_len = line_end - v_start;
            value = malloc(v_len + 1);
            strncpy(value, v_start, v_len);
            value[v_len] = '\0';
            
            // Trim quotes if present
            if (v_len >= 2 && ((value[0] == '"' && value[v_len-1] == '"') || (value[0] == '\'' && value[v_len-1] == '\''))) {
                memmove(value, value + 1, v_len - 2);
                value[v_len - 2] = '\0';
            }
            break;
        }
        
        p = line_end + 1;
    }
    
    return value;
}

// Helper: Parse Nanobot metadata JSON from frontmatter "metadata" field
static cJSON* get_skill_meta(const char* content) {
    char* meta_str = get_frontmatter_value(content, "metadata");
    if (!meta_str) return cJSON_CreateObject(); // Empty object
    
    cJSON* json = cJSON_Parse(meta_str);
    free(meta_str);
    
    if (!json) return cJSON_CreateObject();
    
    cJSON* nanobot = cJSON_GetObjectItem(json, "nanobot");
    if (nanobot) {
        cJSON* copy = cJSON_Duplicate(nanobot, 1);
        cJSON_Delete(json);
        return copy;
    }
    
    cJSON* openclaw = cJSON_GetObjectItem(json, "openclaw");
    if (openclaw) {
        cJSON* copy = cJSON_Duplicate(openclaw, 1);
        cJSON_Delete(json);
        return copy;
    }
    
    cJSON_Delete(json);
    return cJSON_CreateObject();
}

// Helper: Check requirements
static bool check_requirements(cJSON* meta, char** missing_reason) {
    cJSON* requires = cJSON_GetObjectItem(meta, "requires");
    if (!requires) return true;
    
    // Check bins
    cJSON* bins = cJSON_GetObjectItem(requires, "bins");
    if (cJSON_IsArray(bins)) {
        cJSON* bin;
        cJSON_ArrayForEach(bin, bins) {
            if (cJSON_IsString(bin)) {
                if (!command_exists(bin->valuestring)) {
                    if (missing_reason) {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "Missing CLI: %s", bin->valuestring);
                        *missing_reason = strdup(buf);
                    }
                    return false;
                }
            }
        }
    }
    
    // Check env
    cJSON* envs = cJSON_GetObjectItem(requires, "env");
    if (cJSON_IsArray(envs)) {
        cJSON* env;
        cJSON_ArrayForEach(env, envs) {
            if (cJSON_IsString(env)) {
                if (!getenv(env->valuestring)) {
                    if (missing_reason) {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "Missing ENV: %s", env->valuestring);
                        *missing_reason = strdup(buf);
                    }
                    return false;
                }
            }
        }
    }
    
    return true;
}

SkillsLoader* skills_loader_create(const char* workspace) {
    SkillsLoader* loader = malloc(sizeof(SkillsLoader));
    if (!loader) return NULL;

    loader->workspace = strdup(workspace);
    loader->builtin_skills_dir = NULL; // Not used in this version, assuming skills are in workspace

    return loader;
}

void skills_loader_destroy(SkillsLoader* loader) {
    if (!loader) return;

    free(loader->workspace);
    free(loader->builtin_skills_dir);
    free(loader);
}

// Helper to escape XML
static char* escape_xml(const char* s) {
    if (!s) return strdup("");
    // Simplified escape: & < >
    // Calculate len
    size_t len = 0;
    const char* p = s;
    while (*p) {
        if (*p == '&') len += 5;
        else if (*p == '<') len += 4;
        else if (*p == '>') len += 4;
        else len++;
        p++;
    }
    
    char* out = malloc(len + 1);
    char* d = out;
    p = s;
    while (*p) {
        if (*p == '&') { strcpy(d, "&amp;"); d += 5; }
        else if (*p == '<') { strcpy(d, "&lt;"); d += 4; }
        else if (*p == '>') { strcpy(d, "&gt;"); d += 4; }
        else { *d++ = *p; }
        p++;
    }
    *d = 0;
    return out;
}

StringArray* skills_loader_list_skills(SkillsLoader* loader, bool filter_unavailable) {
    StringArray* arr = malloc(sizeof(StringArray));
    *arr = string_array_new();
    
    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/skills", loader->workspace);
    
    DIR* dir = opendir(skills_dir);
    if (!dir) return arr;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR && entry->d_name[0] != '.') {
            char skill_path[1024];
            snprintf(skill_path, sizeof(skill_path), "%s/%s/SKILL.md", skills_dir, entry->d_name);
            
            if (file_exists(skill_path)) {
                if (filter_unavailable) {
                    char* content = read_file(skill_path);
                    if (content) {
                        cJSON* meta = get_skill_meta(content);
                        bool available = check_requirements(meta, NULL);
                        cJSON_Delete(meta);
                        free(content);
                        
                        if (available) {
                            string_array_add(arr, entry->d_name);
                        }
                    }
                } else {
                    string_array_add(arr, entry->d_name);
                }
            }
        }
    }
    
    closedir(dir);
    return arr;
}

char* skills_loader_load_skill(SkillsLoader* loader, const char* name) {
    char skill_path[1024];
    snprintf(skill_path, sizeof(skill_path), "%s/skills/%s/SKILL.md", loader->workspace, name);
    return read_file(skill_path);
}

char* skills_loader_load_skills_for_context(SkillsLoader* loader, StringArray* skill_names) {
    if (!skill_names || skill_names->count == 0) return strdup("");
    
    // Estimate size (rough)
    size_t total_size = 0;
    for (size_t i = 0; i < skill_names->count; i++) {
        char* content = skills_loader_load_skill(loader, skill_names->items[i].data);
        if (content) {
            total_size += strlen(content) + 100; // Headers + margin
            free(content);
        }
    }
    
    if (total_size == 0) return strdup("");
    
    char* result = malloc(total_size + 1);
    result[0] = 0;
    
    for (size_t i = 0; i < skill_names->count; i++) {
        char* content = skills_loader_load_skill(loader, skill_names->items[i].data);
        if (content) {
            // Strip frontmatter
            char* body = content;
            if (strncmp(content, "---\n", 4) == 0) {
                char* end = strstr(content + 4, "\n---\n");
                if (end) body = end + 5;
            }
            
            strcat(result, "### Skill: ");
            strcat(result, skill_names->items[i].data);
            strcat(result, "\n\n");
            strcat(result, body);
            strcat(result, "\n\n---\n\n");
            
            free(content);
        }
    }
    
    // Remove last separator if present
    size_t len = strlen(result);
    if (len > 7 && strcmp(result + len - 7, "\n---\n\n") == 0) {
        result[len - 7] = 0;
    }
    
    return result;
}

char* skills_loader_build_skills_summary(SkillsLoader* loader) {
    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/skills", loader->workspace);
    
    DIR* dir = opendir(skills_dir);
    if (!dir) {
        log_error("Failed to open skills directory: %s", skills_dir);
        return strdup("<skills></skills>");
    }
    
    // Use dynamic string or simple large buffer for now
    size_t cap = 16384;
    char* xml = malloc(cap);
    xml[0] = 0;
    strcat(xml, "<skills>\n");
    
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR && entry->d_name[0] != '.') {
            char skill_path[1024];
            snprintf(skill_path, sizeof(skill_path), "%s/%s/SKILL.md", skills_dir, entry->d_name);
            
            if (file_exists(skill_path)) {
                char* content = read_file(skill_path);
                if (content) {
                    count++;
                    char* desc = get_frontmatter_value(content, "description");
                    if (!desc) desc = strdup(entry->d_name);
                    
                    cJSON* meta = get_skill_meta(content);
                    char* missing = NULL;
                    bool available = check_requirements(meta, &missing);
                    
                    char* name_esc = escape_xml(entry->d_name);
                    char* desc_esc = escape_xml(desc);
                    
                    // Append XML
                    // Check buffer size (rudimentary)
                    if (strlen(xml) + 1000 > cap) {
                        cap *= 2;
                        xml = realloc(xml, cap);
                    }
                    
                    char chunk[2048];
                    snprintf(chunk, sizeof(chunk), 
                        "  <skill available=\"%s\">\n"
                        "    <name>%s</name>\n"
                        "    <description>%s</description>\n"
                        "    <location>%s</location>\n",
                        available ? "true" : "false",
                        name_esc, desc_esc, skill_path);
                        
                    strcat(xml, chunk);
                    
                    if (!available && missing) {
                         char* missing_esc = escape_xml(missing);
                         snprintf(chunk, sizeof(chunk), "    <requires>%s</requires>\n", missing_esc);
                         strcat(xml, chunk);
                         free(missing_esc);
                    }
                    
                    strcat(xml, "  </skill>\n");
                    
                    free(name_esc);
                    free(desc_esc);
                    free(desc);
                    if (missing) free(missing);
                    cJSON_Delete(meta);
                    free(content);
                }
            }
        }
    }
    
    strcat(xml, "</skills>");
    closedir(dir);
    log_info("Loaded %d skills from %s", count, skills_dir);
    return xml;
}

StringArray* skills_loader_get_always_skills(SkillsLoader* loader) {
    StringArray* arr = malloc(sizeof(StringArray));
    *arr = string_array_new();
    
    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/skills", loader->workspace);
    
    DIR* dir = opendir(skills_dir);
    if (!dir) return arr;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR && entry->d_name[0] != '.') {
            char skill_path[1024];
            snprintf(skill_path, sizeof(skill_path), "%s/%s/SKILL.md", skills_dir, entry->d_name);
            
            if (file_exists(skill_path)) {
                char* content = read_file(skill_path);
                if (content) {
                    cJSON* meta = get_skill_meta(content);
                    // Check always flag
                    cJSON* always = cJSON_GetObjectItem(meta, "always");
                    if (cJSON_IsTrue(always)) {
                         if (check_requirements(meta, NULL)) {
                             string_array_add(arr, entry->d_name);
                         }
                    }
                    cJSON_Delete(meta);
                    free(content);
                }
            }
        }
    }
    
    closedir(dir);
    return arr;
}