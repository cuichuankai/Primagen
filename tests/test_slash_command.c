#include "test_framework.h"
#include "../src/agent/agent_common.h"
#include <string.h>

TEST(parse_slash_command_basic) {
    char cmd[64] = {0};
    const char* args = NULL;
    parse_slash_command("/new", cmd, sizeof(cmd), &args);
    ASSERT_EQ_STR("new", cmd);
    ASSERT_TRUE(args == NULL);
}

TEST(parse_slash_command_with_args) {
    char cmd[64] = {0};
    const char* args = NULL;
    parse_slash_command("/new session_name", cmd, sizeof(cmd), &args);
    ASSERT_EQ_STR("new", cmd);
    ASSERT_NOT_NULL(args);
    ASSERT_EQ_STR("session_name", args);
}

TEST(parse_slash_command_help) {
    char cmd[64] = {0};
    const char* args = NULL;
    parse_slash_command("/help", cmd, sizeof(cmd), &args);
    ASSERT_EQ_STR("help", cmd);
    ASSERT_TRUE(args == NULL);
}

TEST(parse_slash_command_with_leading_spaces) {
    char cmd[64] = {0};
    const char* args = NULL;
    parse_slash_command("  /new", cmd, sizeof(cmd), &args);
    ASSERT_EQ_STR("new", cmd);
    ASSERT_TRUE(args == NULL);
}

TEST(parse_slash_command_with_leading_spaces_and_args) {
    char cmd[64] = {0};
    const char* args = NULL;
    parse_slash_command(" \t /help tools", cmd, sizeof(cmd), &args);
    ASSERT_EQ_STR("help", cmd);
    ASSERT_NOT_NULL(args);
    ASSERT_EQ_STR("tools", args);
}

TEST(parse_slash_command_null_input) {
    char cmd[64] = {0};
    const char* args = NULL;
    parse_slash_command(NULL, cmd, sizeof(cmd), &args);
    ASSERT_EQ_STR("", cmd);
}

TEST(parse_slash_command_no_slash) {
    char cmd[64] = {0};
    const char* args = NULL;
    parse_slash_command("hello", cmd, sizeof(cmd), &args);
    ASSERT_EQ_STR("", cmd);
}

TEST(parse_slash_command_only_spaces) {
    char cmd[64] = {0};
    const char* args = NULL;
    parse_slash_command("   ", cmd, sizeof(cmd), &args);
    ASSERT_EQ_STR("", cmd);
}

TEST(parse_slash_command_long_name) {
    char cmd[8] = {0};
    const char* args = NULL;
    parse_slash_command("/verylongcommandname", cmd, sizeof(cmd), &args);
    ASSERT_TRUE(strlen(cmd) < sizeof(cmd));
    ASSERT_EQ_INT(0, cmd[sizeof(cmd) - 1]);
}

TEST(parse_slash_command_reload_plugins) {
    char cmd[64] = {0};
    const char* args = NULL;
    parse_slash_command("/reload-plugins", cmd, sizeof(cmd), &args);
    ASSERT_EQ_STR("reload-plugins", cmd);
    ASSERT_TRUE(args == NULL);
}

TEST_SUITE(slash_command) {
    BEGIN_SUITE(slash_command);
    RUN_TEST(parse_slash_command_basic);
    RUN_TEST(parse_slash_command_with_args);
    RUN_TEST(parse_slash_command_help);
    RUN_TEST(parse_slash_command_with_leading_spaces);
    RUN_TEST(parse_slash_command_with_leading_spaces_and_args);
    RUN_TEST(parse_slash_command_null_input);
    RUN_TEST(parse_slash_command_no_slash);
    RUN_TEST(parse_slash_command_only_spaces);
    RUN_TEST(parse_slash_command_long_name);
    RUN_TEST(parse_slash_command_reload_plugins);
    END_SUITE();
}
