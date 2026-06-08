#include "test_framework.h"
#include "../src/session/session.h"
#include "../src/include/message.h"
#include "../src/include/config.h"
#include "../src/tools/tools_impl.h"
#include "../src/providers/llm_provider.h"
#include "../src/agent/agent_loop.h"
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>

AgentLoop* agent_loop_new(SessionManager* session_mgr, ContextBuilder* ctx_builder, ToolRegistry* tool_reg, MessageBus* bus, Config* config, PluginManager* plugin_mgr, const char* workspace_path) {
    (void)session_mgr; (void)ctx_builder; (void)tool_reg; (void)bus; (void)config; (void)plugin_mgr; (void)workspace_path;
    return NULL;
}

void agent_loop_free(AgentLoop* loop) { (void)loop; }
void agent_loop_run(AgentLoop* loop) { (void)loop; }
void agent_loop_stop(AgentLoop* loop) { (void)loop; }
void agent_loop_set_llm_provider(AgentLoop* loop, LLMProvider* provider) { (void)loop; (void)provider; }

ContextBuilder* context_builder_new(const char* workspace) { (void)workspace; return NULL; }
void context_builder_free(ContextBuilder* cb) { (void)cb; }

static char* make_temp_workspace(const char* suffix) {
    char tmpl[256];
    snprintf(tmpl, sizeof(tmpl), "/tmp/primagen_%s_XXXXXX", suffix);
    char* path = mkdtemp(tmpl);
    return path ? strdup(path) : NULL;
}

static void test_session_persists_tool_chain_messages(void) {
    char* ws = make_temp_workspace("session_tool_chain");
    ASSERT_NOT_NULL(ws, "temp workspace should be created");

    SessionManager* mgr = session_manager_new(ws);
    ASSERT_NOT_NULL(mgr, "session manager should be created");
    Session* session = session_manager_create(mgr, "cli:test_tool_chain");
    ASSERT_NOT_NULL(session, "session should be created");

    Message* user = message_new(ROLE_USER, "read config");
    session_add_message(session, user);

    Message* assistant = message_new(ROLE_ASSISTANT, "");
    message_add_tool_call(assistant, "call_1", "read_file", "{\"path\":\"config.json\"}");
    session_add_message(session, assistant);

    Message* tool = message_new(ROLE_TOOL, "{\"ok\":true}");
    tool->tool_call_id = string_new("call_1");
    tool->name = string_new("read_file");
    session_add_message(session, tool);

    Error save_err = session_manager_save(mgr, session);
    ASSERT_NO_ERROR(save_err, "session save should succeed");
    session_manager_free(mgr);

    SessionManager* mgr2 = session_manager_new(ws);
    Session* loaded = NULL;
    Error load_err = session_manager_load(mgr2, "cli:test_tool_chain", &loaded);
    ASSERT_NO_ERROR(load_err, "session load should succeed");
    ASSERT_NOT_NULL(loaded, "loaded session should exist");
    ASSERT_EQ_SIZE(3, loaded->messages.count, "all tool-chain messages should persist");

    Message* loaded_assistant = *(Message**)dynamic_array_get(&loaded->messages, 1);
    ASSERT_EQ_INT(ROLE_ASSISTANT, loaded_assistant->role, "assistant message should remain assistant");
    ASSERT_EQ_SIZE(1, loaded_assistant->tool_calls_count, "assistant tool call should persist");
    ASSERT_EQ_STR("read_file", loaded_assistant->tool_calls[0].name.data, "tool call name should persist");

    Message* loaded_tool = *(Message**)dynamic_array_get(&loaded->messages, 2);
    ASSERT_EQ_INT(ROLE_TOOL, loaded_tool->role, "tool result should persist");
    ASSERT_EQ_STR("call_1", loaded_tool->tool_call_id.data, "tool_call_id should persist");

    session_manager_free(mgr2);
    free(ws);
}

static void test_restricted_write_allows_new_workspace_file(void) {
    char* ws = make_temp_workspace("tool_write");
    ASSERT_NOT_NULL(ws, "temp workspace should be created");

    Config* cfg = config_create();
    ASSERT_NOT_NULL(cfg, "config should be created");
    cfg->tools.restrict_to_workspace = true;
    cfg->tools.exec.restrict_to_workspace = true;

    ToolContext* ctx = tool_context_new(NULL, NULL, NULL, NULL, NULL, cfg, NULL, ws);
    ASSERT_NOT_NULL(ctx, "tool context should be created");

    String result = string_new("");
    Error err = tool_write_file(ctx, "{\"path\":\"new_file.txt\",\"content\":\"hello\"}", &result);
    ASSERT_NO_ERROR(err, "restricted write should allow new workspace file");
    string_free(&result);

    char path[512];
    snprintf(path, sizeof(path), "%s/new_file.txt", ws);
    FILE* fp = fopen(path, "r");
    ASSERT_NOT_NULL(fp, "written file should exist");
    char buf[16] = {0};
    fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    ASSERT_EQ_STR("hello", buf, "written content should match");

    result = string_new("");
    err = tool_write_file(ctx, "{\"path\":\"/tmp/outside_primagen_test.txt\",\"content\":\"nope\"}", &result);
    ASSERT_EQ_INT((int)ERR_TOOL, (int)err.code, "restricted write should reject outside workspace");
    string_free(&result);

    tool_context_destroy(ctx);
    config_destroy(cfg);
    free(ws);
}

static void test_workspace_restriction_uses_current_workspace(void) {
    char* ws1 = make_temp_workspace("tool_ws_one");
    char* ws2 = make_temp_workspace("tool_ws_two");
    ASSERT_NOT_NULL(ws1, "first temp workspace should be created");
    ASSERT_NOT_NULL(ws2, "second temp workspace should be created");

    Config* cfg = config_create();
    ASSERT_NOT_NULL(cfg, "config should be created");
    cfg->tools.restrict_to_workspace = true;
    cfg->tools.exec.restrict_to_workspace = true;

    ToolContext* ctx1 = tool_context_new(NULL, NULL, NULL, NULL, NULL, cfg, NULL, ws1);
    ToolContext* ctx2 = tool_context_new(NULL, NULL, NULL, NULL, NULL, cfg, NULL, ws2);
    ASSERT_NOT_NULL(ctx1, "first context should be created");
    ASSERT_NOT_NULL(ctx2, "second context should be created");

    String result = string_new("");
    Error err = tool_write_file(ctx1, "{\"path\":\"one.txt\",\"content\":\"one\"}", &result);
    ASSERT_NO_ERROR(err, "first workspace write should succeed");
    string_free(&result);

    result = string_new("");
    err = tool_write_file(ctx2, "{\"path\":\"two.txt\",\"content\":\"two\"}", &result);
    ASSERT_NO_ERROR(err, "second workspace write should not reuse first workspace root");
    string_free(&result);

    char path[512];
    snprintf(path, sizeof(path), "%s/two.txt", ws2);
    FILE* fp = fopen(path, "r");
    ASSERT_NOT_NULL(fp, "second workspace file should exist in second workspace");
    if (fp) fclose(fp);

    tool_context_destroy(ctx1);
    tool_context_destroy(ctx2);
    config_destroy(cfg);
    free(ws1);
    free(ws2);
}

static int mock_shutdown_count = 0;

static Error mock_provider_init(LLMProvider* provider, Config* config) {
    (void)config;
    provider->initialized = true;
    return error_new(ERR_NONE, "");
}

static void mock_provider_shutdown(LLMProvider* provider) {
    mock_shutdown_count++;
    provider->initialized = false;
}

static void test_provider_unregister_by_plugin_shuts_down_before_unload(void) {
    LLMProviderInterface iface = {0};
    iface.name = "mock";
    iface.init = mock_provider_init;
    iface.shutdown = mock_provider_shutdown;

    LLMProviderRegistry* registry = llm_provider_registry_new();
    ASSERT_NOT_NULL(registry, "registry should be created");
    LLMProvider* provider = llm_provider_new(&iface, "mock_plugin_provider");
    ASSERT_NOT_NULL(provider, "provider should be created");
    provider->plugin_ref = (void*)0x1234;
    ASSERT_NO_ERROR(provider->iface->init(provider, NULL), "provider init should succeed");
    ASSERT_EQ_INT(0, llm_provider_registry_register(registry, provider), "provider registration should succeed");
    ASSERT_EQ_SIZE(1, registry->count, "registry should contain provider");

    mock_shutdown_count = 0;
    size_t removed = llm_provider_registry_unregister_by_plugin(registry, (void*)0x1234);
    ASSERT_EQ_SIZE(1, removed, "provider should be unregistered by plugin");
    ASSERT_EQ_INT(1, mock_shutdown_count, "provider shutdown should be called");
    ASSERT_EQ_SIZE(0, registry->count, "registry should be empty");

    llm_provider_registry_free(registry);
}

void run_tests(void) {
    TEST_RUN(test_session_persists_tool_chain_messages);
    TEST_RUN(test_restricted_write_allows_new_workspace_file);
    TEST_RUN(test_workspace_restriction_uses_current_workspace);
    TEST_RUN(test_provider_unregister_by_plugin_shuts_down_before_unload);
}

int main(void) {
    printf("\n=== Runtime Fix Regression Tests ===\n");
    run_tests();
    print_test_summary();
    return get_test_exit_code();
}
