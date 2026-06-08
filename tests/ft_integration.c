#include "test_framework.h"
#include "../src/bus/message_bus.h"
#include "../src/session/session.h"
#include "../src/tools/tool.h"
#include "../src/tools/tool_executor.h"
#include "../src/include/config.h"
#include "../src/include/message.h"
#include "../src/include/utils.h"
#include "../src/vendor/cJSON/cJSON.h"
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

TEST(ft_bus_session_integration) {
    MessageBus* bus = message_bus_new();
    SessionManager* mgr = session_manager_new("/tmp/ft_bus_session");
    ASSERT_NOT_NULL(bus);
    ASSERT_NOT_NULL(mgr);
    
    Session* s = session_manager_create(mgr, "cli:user1");
    ASSERT_NOT_NULL(s);
    
    InboundMessage* in_msg = inbound_message_new("cli", "user1", "Hello agent");
    message_bus_send_inbound(bus, in_msg);
    
    InboundMessage* recv = message_bus_receive_inbound(bus);
    ASSERT_NOT_NULL(recv);
    ASSERT_EQ_STR("cli", recv->channel.data);
    ASSERT_EQ_STR("user1", recv->chat_id.data);
    ASSERT_EQ_STR("Hello agent", recv->content.data);
    
    Message* m = message_new(ROLE_USER, recv->content.data);
    session_add_message(s, m);
    ASSERT_EQ_SIZE(1, s->messages.count);
    
    inbound_message_free(recv);
    session_manager_free(mgr);
    message_bus_free(bus);
}

TEST(ft_config_validate_integration) {
    Config* cfg = config_create();
    ASSERT_NOT_NULL(cfg);
    
    Error err = config_validate(cfg);
    ASSERT_EQ_INT(ERR_NONE, err.code);
    
    const char* path = "/tmp/ft_config_test.json";
    bool saved = config_save_to_file(cfg, path);
    ASSERT_TRUE(saved);
    
    Config* cfg2 = config_create();
    bool loaded = config_load_from_file(cfg2, path);
    ASSERT_TRUE(loaded);
    
    Error err2 = config_validate(cfg2);
    ASSERT_EQ_INT(ERR_NONE, err2.code);
    
    ASSERT_EQ_STR(cfg->agent.model, cfg2->agent.model);
    ASSERT_EQ_INT(cfg->agent.max_tokens, cfg2->agent.max_tokens);
    
    config_destroy(cfg);
    config_destroy(cfg2);
    remove(path);
}

TEST(ft_config_invalid_then_fix) {
    Config* cfg = config_create();
    cfg->agent.temperature = 5.0;
    Error err = config_validate(cfg);
    ASSERT_TRUE(err.code != ERR_NONE);
    
    cfg->agent.temperature = 1.0;
    err = config_validate(cfg);
    ASSERT_EQ_INT(ERR_NONE, err.code);
    
    free(cfg->agent.model);
    cfg->agent.model = strdup("");
    err = config_validate(cfg);
    ASSERT_TRUE(err.code != ERR_NONE);
    
    free(cfg->agent.model);
    cfg->agent.model = strdup("gpt-4");
    err = config_validate(cfg);
    ASSERT_EQ_INT(ERR_NONE, err.code);
    
    config_destroy(cfg);
}

static Error ft_echo_tool_fn(void* user_data, const char* args_json, String* result) {
    (void)user_data;
    string_append(result, args_json ? args_json : "");
    return error_new(ERR_NONE, "");
}

TEST(ft_tool_registry_executor_integration) {
    ToolRegistry* reg = tool_registry_new();
    ASSERT_NOT_NULL(reg);
    
    tool_registry_register(reg, "echo", "Echo tool", "{}", ft_echo_tool_fn, NULL);
    
    ToolExecutor* executor = tool_executor_new(reg, 2);
    ASSERT_NOT_NULL(executor);
    
    String result = string_new("");
    Error err = tool_executor_execute_sync(executor, "echo", "{\"msg\":\"hi\"}", &result, 5000);
    ASSERT_EQ_INT(ERR_NONE, err.code);
    
    string_free(&result);
    tool_executor_destroy(executor);
    tool_registry_free(reg);
}

TEST(ft_session_save_load_integration) {
    const char* ws = "/tmp/ft_session_save";
    mkdir(ws, 0755);
    char sessions_dir[512];
    snprintf(sessions_dir, sizeof(sessions_dir), "%s/sessions", ws);
    mkdir(sessions_dir, 0755);
    
    SessionManager* mgr1 = session_manager_new(ws);
    Session* s1 = session_manager_create(mgr1, "test:integration");
    ASSERT_NOT_NULL(s1);
    
    Message* m1 = message_new(ROLE_USER, "What is AI?");
    session_add_message(s1, m1);
    Message* m2 = message_new(ROLE_ASSISTANT, "AI is artificial intelligence");
    session_add_message(s1, m2);
    ASSERT_EQ_SIZE(2, s1->messages.count);
    
    Error save_err = session_manager_save(mgr1, s1);
    ASSERT_EQ_INT(ERR_NONE, save_err.code);
    
    session_manager_free(mgr1);
    
    SessionManager* mgr2 = session_manager_new(ws);
    Session* s2 = NULL;
    Error load_err = session_manager_load(mgr2, "test:integration", &s2);
    ASSERT_EQ_INT(ERR_NONE, load_err.code);
    ASSERT_NOT_NULL(s2);
    ASSERT_EQ_SIZE(2, s2->messages.count);
    
    session_manager_free(mgr2);
}

TEST(ft_bus_internal_event_lifecycle) {
    MessageBus* bus = message_bus_new();
    ASSERT_NOT_NULL(bus);
    
    ToolCall tc = {string_new("call_1"), string_new("read_file"), string_new("{\"path\":\"/tmp/test\"}")};
    Error llm_err = error_new(ERR_NONE, "");
    InternalEvent* ev = internal_event_new_llm_result("sess1", llm_err, "I need to read a file", &tc, 1);
    string_free(&tc.id);
    string_free(&tc.name);
    string_free(&tc.arguments);
    
    message_bus_send_internal(bus, ev);
    
    InternalEvent* recv = message_bus_receive_internal_timed(bus, 1000);
    ASSERT_NOT_NULL(recv);
    ASSERT_EQ_INT(EVENT_LLM_RESULT, recv->type);
    ASSERT_EQ_STR("sess1", recv->session_key.data);
    ASSERT_EQ_SIZE(1, recv->tool_calls_count);
    ASSERT_EQ_STR("read_file", recv->tool_calls[0].name.data);
    
    Error tool_err = error_new(ERR_NONE, "");
    InternalEvent* tool_ev = internal_event_new_tool_result("sess1", "call_1", "read_file", "file contents here", tool_err);
    message_bus_send_internal(bus, tool_ev);
    
    InternalEvent* tool_recv = message_bus_receive_internal_timed(bus, 1000);
    ASSERT_NOT_NULL(tool_recv);
    ASSERT_EQ_INT(EVENT_TOOL_RESULT, tool_recv->type);
    ASSERT_EQ_STR("call_1", tool_recv->tool_call_id.data);
    ASSERT_EQ_STR("file contents here", tool_recv->tool_result.data);
    
    internal_event_free(recv);
    internal_event_free(tool_recv);
    message_bus_free(bus);
}

TEST(ft_estimate_tokens_message_chain) {
    size_t t1 = estimate_message_tokens("user", "Hello, how are you?", 0);
    size_t t2 = estimate_message_tokens("assistant", "I'm doing well!", 0);
    size_t t3 = estimate_message_tokens("assistant", "Let me search for that", 1);
    
    ASSERT_TRUE(t1 > 0);
    ASSERT_TRUE(t2 > 0);
    ASSERT_TRUE(t3 > t2);
    
    size_t total = t1 + t2 + t3;
    ASSERT_TRUE(total > 0);
}

TEST(ft_strip_think_tags_then_estimate) {
    char* raw = strdup("Hello World");
    char* cleaned = strip_think_tags(raw);
    ASSERT_NOT_NULL(cleaned);
    
    size_t tokens = estimate_tokens(cleaned);
    ASSERT_TRUE(tokens > 0);
    
    free(cleaned);
    free(raw);
}

TEST(ft_config_plugin_lifecycle) {
    Config* cfg = config_create();
    ASSERT_NOT_NULL(cfg);
    
    PluginConfig* pc1 = config_add_plugin_config(cfg, "web_tools");
    ASSERT_NOT_NULL(pc1);
    ASSERT_EQ_STR("web_tools", pc1->plugin_id);
    ASSERT_FALSE(pc1->enabled);
    
    pc1->enabled = true;
    pc1->config = cJSON_CreateObject();
    cJSON_AddStringToObject(pc1->config, "proxy", "http://localhost:8080");
    
    PluginConfig* found = config_get_plugin_config(cfg, "web_tools");
    ASSERT_NOT_NULL(found);
    ASSERT_TRUE(found->enabled);
    
    PluginConfig* not_found = config_get_plugin_config(cfg, "nonexistent");
    ASSERT_NULL(not_found);
    
    const char* path = "/tmp/ft_plugin_config.json";
    bool saved = config_save_to_file(cfg, path);
    ASSERT_TRUE(saved);
    
    Config* cfg2 = config_create();
    config_load_from_file(cfg2, path);
    
    PluginConfig* pc2 = config_get_plugin_config(cfg2, "web_tools");
    ASSERT_NOT_NULL(pc2);
    ASSERT_TRUE(pc2->enabled);
    
    config_destroy(cfg);
    config_destroy(cfg2);
    remove(path);
}

TEST(ft_generate_id_format) {
    char* id1 = generate_id("session");
    char* id2 = generate_id("msg");
    char* id3 = generate_id(NULL);
    
    ASSERT_NOT_NULL(id1);
    ASSERT_NOT_NULL(id2);
    ASSERT_NOT_NULL(id3);
    
    ASSERT_TRUE(strncmp(id1, "session_", 8) == 0);
    ASSERT_TRUE(strncmp(id2, "msg_", 4) == 0);
    ASSERT_TRUE(strncmp(id3, "id_", 3) == 0);
    
    ASSERT_TRUE(strcmp(id1, id2) != 0);
    ASSERT_TRUE(strcmp(id1, id3) != 0);
    
    free(id1);
    free(id2);
    free(id3);
}

TEST_SUITE(integration) {
    BEGIN_SUITE(integration);
    RUN_TEST(ft_bus_session_integration);
    RUN_TEST(ft_config_validate_integration);
    RUN_TEST(ft_config_invalid_then_fix);
    RUN_TEST(ft_tool_registry_executor_integration);
    RUN_TEST(ft_session_save_load_integration);
    RUN_TEST(ft_bus_internal_event_lifecycle);
    RUN_TEST(ft_estimate_tokens_message_chain);
    RUN_TEST(ft_strip_think_tags_then_estimate);
    RUN_TEST(ft_config_plugin_lifecycle);
    RUN_TEST(ft_generate_id_format);
    END_SUITE();
}
