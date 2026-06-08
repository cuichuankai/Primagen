#include "test_framework.h"
#include "../src/include/common.h"
#include "../src/include/plugin.h"
#include "../src/providers/llm_provider.h"

static void test_registry_create_and_free() {
    LLMProviderRegistry* registry = llm_provider_registry_new();
    ASSERT_NOT_NULL(registry, "registry should not be NULL");
    ASSERT_EQ_SIZE(0, registry->count, "new registry should have 0 providers");

    llm_provider_registry_free(registry);
}

static void test_provider_create_and_free() {
    LLMProviderInterface mock_iface = {0};
    mock_iface.name = "mock_provider";

    LLMProvider* provider = llm_provider_new(&mock_iface, "mock");
    ASSERT_NOT_NULL(provider, "provider should not be NULL");
    ASSERT_NOT_NULL(provider->name, "provider name should not be NULL");
    ASSERT_EQ_STR("mock", provider->name, "provider name mismatch");
    ASSERT_TRUE(provider->iface == &mock_iface, "provider iface pointer mismatch");

    llm_provider_free(provider);
}

static void test_registry_register_and_find() {
    LLMProviderRegistry* registry = llm_provider_registry_new();
    ASSERT_NOT_NULL(registry, "registry should not be NULL");

    LLMProviderInterface mock_iface = {0};
    mock_iface.name = "mock_interface";

    LLMProvider* provider = llm_provider_new(&mock_iface, "mock_provider");
    ASSERT_NOT_NULL(provider, "provider should not be NULL");

    int ret = llm_provider_registry_register(registry, provider);
    ASSERT_EQ_INT(0, ret, "register should succeed");
    ASSERT_EQ_SIZE(1, registry->count, "registry should have 1 provider");

    LLMProvider* found = llm_provider_registry_find(registry, "mock_provider");
    ASSERT_NOT_NULL(found, "should find registered provider");
    ASSERT_EQ_STR("mock_provider", found->name, "found provider name mismatch");

    LLMProvider* not_found = llm_provider_registry_find(registry, "nonexistent");
    ASSERT_NULL(not_found, "should not find nonexistent provider");

    llm_provider_registry_free(registry);
}

static void test_registry_register_duplicate() {
    LLMProviderRegistry* registry = llm_provider_registry_new();
    ASSERT_NOT_NULL(registry, "registry should not be NULL");

    LLMProviderInterface mock_iface = {0};

    LLMProvider* p1 = llm_provider_new(&mock_iface, "provider1");
    ASSERT_NOT_NULL(p1, "first provider should not be NULL");

    LLMProvider* p2 = llm_provider_new(&mock_iface, "provider1");
    ASSERT_NOT_NULL(p2, "second provider should not be NULL");

    int ret1 = llm_provider_registry_register(registry, p1);
    ASSERT_EQ_INT(0, ret1, "first register should succeed");

    int ret2 = llm_provider_registry_register(registry, p2);
    ASSERT_EQ_INT(0, ret2, "duplicate register should return 0");

    ASSERT_EQ_SIZE(1, registry->count, "should still have only 1 provider after duplicate");

    // p2 should have been freed by register since it returned 0 but didn't add
    llm_provider_registry_free(registry);
}

static void test_registry_set_and_get_active() {
    LLMProviderRegistry* registry = llm_provider_registry_new();
    ASSERT_NOT_NULL(registry, "registry should not be NULL");

    LLMProviderInterface mock_iface = {0};

    LLMProvider* p1 = llm_provider_new(&mock_iface, "provider1");
    LLMProvider* p2 = llm_provider_new(&mock_iface, "provider2");

    llm_provider_registry_register(registry, p1);
    llm_provider_registry_register(registry, p2);

    // Check initial active is NULL
    LLMProvider* initial_active = llm_provider_registry_get_active(registry);
    ASSERT_NULL(initial_active, "initial active should be NULL");

    // Set active to provider1
    int ret = llm_provider_registry_set_active(registry, "provider1");
    ASSERT_EQ_INT(0, ret, "set active should succeed");

    LLMProvider* active = llm_provider_registry_get_active(registry);
    ASSERT_NOT_NULL(active, "active should not be NULL after set");
    ASSERT_EQ_STR("provider1", active->name, "active provider name mismatch");

    // Set active to provider2
    ret = llm_provider_registry_set_active(registry, "provider2");
    ASSERT_EQ_INT(0, ret, "set active to provider2 should succeed");

    active = llm_provider_registry_get_active(registry);
    ASSERT_NOT_NULL(active, "active should not be NULL");
    ASSERT_EQ_STR("provider2", active->name, "active provider name mismatch after switch");

    // Set active to nonexistent
    ret = llm_provider_registry_set_active(registry, "nonexistent");
    ASSERT_EQ_INT(-1, ret, "set active to nonexistent should fail");

    // Active should still be provider2
    active = llm_provider_registry_get_active(registry);
    ASSERT_EQ_STR("provider2", active->name, "active should remain provider2 after failed set");

    llm_provider_registry_free(registry);
}

static void test_registry_null_handling() {
    // NULL registry
    LLMProvider* ret = llm_provider_registry_find(NULL, "test");
    ASSERT_NULL(ret, "find on NULL registry should return NULL");

    ret = llm_provider_registry_get_active(NULL);
    ASSERT_NULL(ret, "get_active on NULL registry should return NULL");

    // NULL name
    LLMProviderRegistry* registry = llm_provider_registry_new();
    ASSERT_NOT_NULL(registry, "registry should not be NULL");

    ret = llm_provider_registry_find(registry, NULL);
    ASSERT_NULL(ret, "find with NULL name should return NULL");

    int int_ret = llm_provider_registry_set_active(registry, NULL);
    ASSERT_EQ_INT(-1, int_ret, "set_active with NULL name should fail");

    // Register NULL provider
    int_ret = llm_provider_registry_register(registry, NULL);
    ASSERT_EQ_INT(-1, int_ret, "register NULL provider should fail");

    // NULL iface should be rejected
    LLMProvider* null_iface = llm_provider_new(NULL, "test");
    ASSERT_NULL(null_iface, "provider with NULL iface should be rejected");

    // NULL name for provider - should create with empty name
    LLMProviderInterface mock_iface = {0};
    LLMProvider* unnamed = llm_provider_new(&mock_iface, NULL);
    ASSERT_NOT_NULL(unnamed, "provider with NULL name should still be creatable");
    if (unnamed) llm_provider_free(unnamed);

    llm_provider_registry_free(registry);
}

static void test_registry_multiple_providers() {
    LLMProviderRegistry* registry = llm_provider_registry_new();
    ASSERT_NOT_NULL(registry, "registry should not be NULL");

    LLMProviderInterface mock_iface = {0};

    const char* names[] = {"alpha", "beta", "gamma", "delta", "epsilon"};
    const size_t num = sizeof(names) / sizeof(names[0]);

    for (size_t i = 0; i < num; i++) {
        LLMProvider* p = llm_provider_new(&mock_iface, names[i]);
        ASSERT_NOT_NULL(p, "provider should not be NULL");
        int ret = llm_provider_registry_register(registry, p);
        ASSERT_EQ_INT(0, ret, "register should succeed");
    }

    ASSERT_EQ_SIZE(num, registry->count, "registry should have correct count");

    for (size_t i = 0; i < num; i++) {
        LLMProvider* found = llm_provider_registry_find(registry, names[i]);
        ASSERT_NOT_NULL(found, "should find provider");
        ASSERT_EQ_STR(names[i], found->name, "found provider name mismatch");
    }

    llm_provider_registry_free(registry);
}

static void test_provider_init_shutdown_lifecycle() {
    LLMProviderInterface mock_iface = {0};
    mock_iface.name = "lifecycle_test";
    mock_iface.init = NULL;
    mock_iface.shutdown = NULL;

    LLMProvider* p = llm_provider_new(&mock_iface, "lifecycle");
    ASSERT_NOT_NULL(p, "provider should be created");
    ASSERT_FALSE(p->initialized, "provider should not be initialized");
    ASSERT_TRUE(p->iface == &mock_iface, "iface pointer should match");

    llm_provider_free(p);
}

void run_tests(void) {
    TEST_RUN(test_registry_create_and_free);
    TEST_RUN(test_provider_create_and_free);
    TEST_RUN(test_registry_register_and_find);
    TEST_RUN(test_registry_register_duplicate);
    TEST_RUN(test_registry_set_and_get_active);
    TEST_RUN(test_registry_null_handling);
    TEST_RUN(test_registry_multiple_providers);
    TEST_RUN(test_provider_init_shutdown_lifecycle);
}

int main(void) {
    printf("\n=== LLM Provider Registry Unit Tests ===\n");
    run_tests();
    print_test_summary();
    return get_test_exit_code();
}