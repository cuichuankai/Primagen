#include "test_framework.h"
#include "../src/include/common.h"
#include "../src/include/config.h"

static void test_config_create_defaults() {
    Config* cfg = config_create();
    ASSERT_NOT_NULL(cfg, "config should not be NULL");

    // Check agent config defaults
    ASSERT_EQ_STR("openai", cfg->agent.provider, "default provider name mismatch");
    ASSERT_EQ_INT(15, cfg->agent.max_tool_iterations, "default max_tool_iterations mismatch");
    ASSERT_EQ_INT(100, cfg->agent.memory_window, "default memory_window mismatch");
    ASSERT_EQ_INT(4000, cfg->agent.memory_max_tokens, "default memory_max_tokens mismatch");
    ASSERT_EQ_DOUBLE(0.8, cfg->agent.memory_consolidation_threshold, 0.001, "default consolidation threshold mismatch");

    // Check that default openai provider was auto-added
    ASSERT_TRUE(cfg->providers.count >= 1, "should have at least 1 provider auto-added");

    ProviderConfig* pc = config_get_provider(cfg, "openai");
    ASSERT_NOT_NULL(pc, "default openai provider should exist");
    ASSERT_NOT_NULL(pc->name, "provider name should not be NULL");
    ASSERT_EQ_STR("openai", pc->name, "default provider name mismatch");
    ASSERT_NOT_NULL(pc->model, "provider model should not be NULL");
    ASSERT_NOT_NULL(pc->api_base, "provider api_base should not be NULL");

    // Check tools config defaults
    ASSERT_EQ_INT(300, cfg->tools.exec.timeout, "default tool timeout mismatch");
    ASSERT_TRUE(cfg->tools.exec.restrict_to_workspace, "default restrict_to_workspace mismatch");

    // Check log config defaults
    ASSERT_EQ_STR("INFO", cfg->log.level, "default log level mismatch");
    ASSERT_FALSE(cfg->log.console_output, "default console_output mismatch");

    config_destroy(cfg);
}

static void test_provider_add_and_get() {
    Config* cfg = config_create();
    ASSERT_NOT_NULL(cfg, "config should not be NULL");

    size_t initial_count = cfg->providers.count;

    // Add a new provider
    ProviderConfig* pc = config_add_provider(cfg, "anthropic");
    ASSERT_NOT_NULL(pc, "added provider should not be NULL");
    ASSERT_EQ_STR("anthropic", pc->name, "provider name mismatch");
    ASSERT_EQ_SIZE(initial_count + 1, cfg->providers.count, "provider count should increase");

    // Set some fields
    free(pc->model);
    pc->model = strdup("claude-3-opus");
    free(pc->api_base);
    pc->api_base = strdup("https://api.anthropic.com/v1");

    // Get it back
    ProviderConfig* found = config_get_provider(cfg, "anthropic");
    ASSERT_NOT_NULL(found, "should find added provider");
    ASSERT_EQ_STR("anthropic", found->name, "found provider name mismatch");
    ASSERT_EQ_STR("claude-3-opus", found->model, "found provider model mismatch");
    ASSERT_EQ_STR("https://api.anthropic.com/v1", found->api_base, "found provider api_base mismatch");

    // Add same provider again - should return existing
    ProviderConfig* same = config_add_provider(cfg, "anthropic");
    ASSERT_NOT_NULL(same, "adding existing provider should return it");
    ASSERT_TRUE(same == found, "should return same pointer for existing provider");
    ASSERT_EQ_SIZE(initial_count + 1, cfg->providers.count, "count should not increase for duplicate");

    config_destroy(cfg);
}

static void test_provider_add_multiple() {
    Config* cfg = config_create();

    const char* names[] = {"alpha", "beta", "gamma", "delta"};
    const size_t num = sizeof(names) / sizeof(names[0]);

    // Add multiple providers, skip the first which is "openai" (already added by default)
    for (size_t i = 0; i < num; i++) {
        ProviderConfig* pc = config_add_provider(cfg, names[i]);
        ASSERT_NOT_NULL(pc, "added provider should not be NULL");
        ASSERT_EQ_STR(names[i], pc->name, "provider name mismatch");
    }

    // Verify all are found (note: "openai" was auto-created, so it exists too)
    for (size_t i = 0; i < num; i++) {
        ProviderConfig* pc = config_get_provider(cfg, names[i]);
        ASSERT_NOT_NULL(pc, "should find provider");
        ASSERT_EQ_STR(names[i], pc->name, "found name mismatch");
    }

    config_destroy(cfg);
}

static void test_active_provider() {
    Config* cfg = config_create();
    ASSERT_NOT_NULL(cfg, "config should not be NULL");

    // Default active provider should be "openai"
    ProviderConfig* active = config_get_active_provider(cfg);
    ASSERT_NOT_NULL(active, "active provider should not be NULL");
    ASSERT_EQ_STR("openai", active->name, "default active provider should be openai");

    // Add another provider and switch to it
    ProviderConfig* pc = config_add_provider(cfg, "deepseek");
    ASSERT_NOT_NULL(pc, "added provider should not be NULL");
    free(pc->model);
    pc->model = strdup("deepseek-chat");

    free(cfg->agent.provider);
    cfg->agent.provider = strdup("deepseek");

    active = config_get_active_provider(cfg);
    ASSERT_NOT_NULL(active, "active provider should not be NULL after switch");
    ASSERT_EQ_STR("deepseek", active->name, "active provider should be deepseek");

    // Switch to nonexistent provider
    free(cfg->agent.provider);
    cfg->agent.provider = strdup("nonexistent");
    active = config_get_active_provider(cfg);
    ASSERT_NULL(active, "active provider for nonexistent should be NULL");

    config_destroy(cfg);
}

static void test_provider_null_handling() {
    Config* cfg = config_create();

    // NULL config
    ProviderConfig* pc = config_add_provider(NULL, "test");
    ASSERT_NULL(pc, "add_provider with NULL config should return NULL");

    pc = config_get_provider(NULL, "test");
    ASSERT_NULL(pc, "get_provider with NULL config should return NULL");

    pc = config_get_active_provider(NULL);
    ASSERT_NULL(pc, "get_active_provider with NULL config should return NULL");

    // NULL name
    pc = config_add_provider(cfg, NULL);
    ASSERT_NULL(pc, "add_provider with NULL name should return NULL");

    pc = config_get_provider(cfg, NULL);
    ASSERT_NULL(pc, "get_provider with NULL name should return NULL");

    config_destroy(cfg);
}

static void test_provider_config_free_safely() {
    // Test that freeing individual fields doesn't crash
    Config* cfg = config_create();

    ProviderConfig* pc = config_add_provider(cfg, "test_provider");
    ASSERT_NOT_NULL(pc, "added provider should not be NULL");

    // Set specific values
    free(pc->model);
    pc->model = strdup("test-model");
    free(pc->api_key);
    pc->api_key = strdup("sk-test-key");
    free(pc->api_base);
    pc->api_base = strdup("https://test.api.com/v1");
    free(pc->reasoning_effort);
    pc->reasoning_effort = strdup("high");

    // Verify these were set
    ASSERT_EQ_STR("test-model", pc->model, "model mismatch");
    ASSERT_EQ_STR("sk-test-key", pc->api_key, "api_key mismatch");
    ASSERT_EQ_STR("https://test.api.com/v1", pc->api_base, "api_base mismatch");
    ASSERT_EQ_STR("high", pc->reasoning_effort, "reasoning_effort mismatch");

    config_destroy(cfg);
}

void run_tests(void) {
    TEST_RUN(test_config_create_defaults);
    TEST_RUN(test_provider_add_and_get);
    TEST_RUN(test_provider_add_multiple);
    TEST_RUN(test_active_provider);
    TEST_RUN(test_provider_null_handling);
    TEST_RUN(test_provider_config_free_safely);
}

int main(void) {
    printf("\n=== Config Provider Management Unit Tests ===\n");
    run_tests();
    print_test_summary();
    return get_test_exit_code();
}