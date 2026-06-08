#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

int test_plugin_load(const char* so_path) {
    void* handle = dlopen(so_path, RTLD_LAZY);
    if (!handle) {
        printf("FAIL: Cannot load %s: %s\n", so_path, dlerror());
        return 1;
    }

    typedef void* (*GetInfoFunc)(void);
    GetInfoFunc get_info = (GetInfoFunc)dlsym(handle, "plugin_get_info");
    if (!get_info) {
        printf("FAIL: Cannot find plugin_get_info: %s\n", dlerror());
        dlclose(handle);
        return 1;
    }

    void* info_ptr = get_info();
    if (!info_ptr) {
        printf("FAIL: plugin_get_info returned NULL\n");
        dlclose(handle);
        return 1;
    }

    typedef struct {
        int version;
        int type;
        const char* name;
        const char* description;
        const char* plugin_id;
        void* metadata;
        void* (*get_default_config)(void);
    } TestPluginInfo;

    TestPluginInfo* info = (TestPluginInfo*)info_ptr;
    printf("  name: %s\n", info->name);
    printf("  type: %d\n", info->type);
    printf("  plugin_id: %s\n", info->plugin_id);
    printf("  description: %s\n", info->description);

    if (info->type != 4) {
        printf("FAIL: Expected type PLUGIN_LLM_PROVIDER (4), got %d\n", info->type);
        dlclose(handle);
        return 1;
    }

    if (strcmp(info->plugin_id, "mnn_provider") != 0) {
        printf("FAIL: Expected plugin_id 'mnn_provider', got '%s'\n", info->plugin_id);
        dlclose(handle);
        return 1;
    }

    if (info->get_default_config) {
        printf("  get_default_config: present (skipped, requires cJSON)\n");
    }

    dlclose(handle);
    printf("PASS: Plugin load test\n");
    return 0;
}

int main(int argc, char* argv[]) {
    const char* so_path = argc > 1 ? argv[1] : "./mnn_provider.so";
    int failures = 0;
    printf("=== MNN Provider Plugin Tests ===\n\n");

    printf("Test 1: Plugin load and info (path: %s)\n", so_path);
    failures += test_plugin_load(so_path);

    printf("\n=== Results: %d failures ===\n", failures);
    return failures;
}
