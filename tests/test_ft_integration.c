#include "test_framework.h"
#include "../src/include/common.h"
#include "../src/include/config.h"
#include "../src/include/plugin.h"
#include "../src/providers/llm_provider.h"

// =============================================================================
// End-to-end: Config -> Provider -> Registry integration
// =============================================================================

static void test_config_to_registry_integration() {
    // Create config with multiple providers
    Config* cfg = config_create();
    ASSERT_NOT_NULL(cfg, "config should not be NULL");

    // Add a second provider
    ProviderConfig* pc = config_add_provider(cfg, "anthropic");
    ASSERT_NOT_NULL(pc, "added provider should not be NULL");
    free(pc->model);
    pc->model = strdup("claude-3-opus");
    free(pc->api_base);
    pc->api_base = strdup("https://api.anthropic.com/v1");

    // Create registry and register providers from config
    LLMProviderRegistry* registry = llm_provider_registry_new();
    ASSERT_NOT_NULL(registry, "registry should not be NULL");

    // Register providers based on config entries
    for (size_t i = 0; i < cfg->providers.count; i++) {
        ProviderConfig* cfg_pc = &cfg->providers.items[i];
        ASSERT_NOT_NULL(cfg_pc->name, "provider name should not be NULL");
    }

    // Verify config integrity
    ASSERT_TRUE(cfg->providers.count >= 2, "should have at least 2 providers in config");
    ASSERT_NOT_NULL(config_get_provider(cfg, "openai"), "openai should exist");
    ASSERT_NOT_NULL(config_get_provider(cfg, "anthropic"), "anthropic should exist");

    llm_provider_registry_free(registry);
    config_destroy(cfg);
}

static void test_provider_switch_scenario() {
    Config* cfg = config_create();
    ASSERT_NOT_NULL(cfg, "config should not be NULL");

    // Simulate scenario: user switches from openai to anthropic
    ProviderConfig* pc = config_add_provider(cfg, "anthropic");
    ASSERT_NOT_NULL(pc, "added anthropic provider should not be NULL");
    free(pc->model);
    pc->model = strdup("claude-3-sonnet");
    free(pc->api_base);
    pc->api_base = strdup("https://api.anthropic.com/v1");
    pc->temperature = 0.3;
    pc->max_tokens = 8192;

    // Verify openai is still active
    ProviderConfig* active = config_get_active_provider(cfg);
    ASSERT_NOT_NULL(active, "active provider should not be NULL");
    ASSERT_EQ_STR("openai", active->name, "active should be openai initially");

    // Switch to anthropic
    free(cfg->agent.provider);
    cfg->agent.provider = strdup("anthropic");

    // Verify switch
    active = config_get_active_provider(cfg);
    ASSERT_NOT_NULL(active, "active provider should not be NULL after switch");
    ASSERT_EQ_STR("anthropic", active->name, "active should be anthropic");
    ASSERT_EQ_STR("claude-3-sonnet", active->model, "model should be claude-3-sonnet");
    ASSERT_EQ_DOUBLE(0.3, active->temperature, 0.001, "temperature mismatch");
    ASSERT_EQ_INT(8192, active->max_tokens, "max_tokens mismatch");

    // Switch back to openai
    free(cfg->agent.provider);
    cfg->agent.provider = strdup("openai");

    active = config_get_active_provider(cfg);
    ASSERT_NOT_NULL(active, "active provider should not be NULL after switch back");
    ASSERT_EQ_STR("openai", active->name, "active should be openai");

    config_destroy(cfg);
}

// Test registry initialization with multiple providers
static void test_full_registry_lifecycle() {
    LLMProviderRegistry* registry = llm_provider_registry_new();
    ASSERT_NOT_NULL(registry, "registry should not be NULL");

    LLMProviderInterface mock_iface1 = {0};
    mock_iface1.name = "interface_a";
    LLMProviderInterface mock_iface2 = {0};
    mock_iface2.name = "interface_b";

    LLMProvider* p1 = llm_provider_new(&mock_iface1, "provider_a");
    LLMProvider* p2 = llm_provider_new(&mock_iface2, "provider_b");
    LLMProvider* p3 = llm_provider_new(&mock_iface1, "provider_c");

    ASSERT_NOT_NULL(p1, "p1 should not be NULL");
    ASSERT_NOT_NULL(p2, "p2 should not be NULL");
    ASSERT_NOT_NULL(p3, "p3 should not be NULL");

    int ret = llm_provider_registry_register(registry, p1);
    ASSERT_EQ_INT(0, ret, "register p1 should succeed");
    ret = llm_provider_registry_register(registry, p2);
    ASSERT_EQ_INT(0, ret, "register p2 should succeed");
    ret = llm_provider_registry_register(registry, p3);
    ASSERT_EQ_INT(0, ret, "register p3 should succeed");

    ASSERT_EQ_SIZE(3, registry->count, "should have 3 providers");

    ret = llm_provider_registry_set_active(registry, "provider_b");
    ASSERT_EQ_INT(0, ret, "set active to provider_b should succeed");

    LLMProvider* active = llm_provider_registry_get_active(registry);
    ASSERT_NOT_NULL(active, "active should not be NULL");
    ASSERT_EQ_STR("provider_b", active->name, "active should be provider_b");

    llm_provider_registry_free(registry);
}

// Test provider interface propagation
static void test_provider_interface_propagation() {
    LLMProviderInterface mock_iface = {0};
    mock_iface.name = "test_interface";

    LLMProvider* p = llm_provider_new(&mock_iface, "test_provider");
    ASSERT_NOT_NULL(p, "provider should not be NULL");
    ASSERT_TRUE(p->iface == &mock_iface, "iface pointer should be preserved");
    ASSERT_FALSE(p->initialized, "new provider should not be initialized");

    LLMProviderRegistry* registry = llm_provider_registry_new();
    llm_provider_registry_register(registry, p);
    llm_provider_registry_set_active(registry, "test_provider");

    LLMProvider* active = llm_provider_registry_get_active(registry);
    ASSERT_NOT_NULL(active, "active should not be NULL");
    ASSERT_TRUE(active->iface == &mock_iface, "active provider iface should match");

    llm_provider_registry_free(registry);
}

void run_tests(void) {
    TEST_RUN(test_config_to_registry_integration);
    TEST_RUN(test_provider_switch_scenario);
    TEST_RUN(test_full_registry_lifecycle);
    TEST_RUN(test_provider_interface_propagation);
}

int main(void) {
    printf("\n=== Functional Integration Tests ===\n");
    run_tests();
    print_test_summary();
    return get_test_exit_code();
}