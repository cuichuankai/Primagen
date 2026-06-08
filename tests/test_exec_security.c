#include "test_framework.h"
#include <string.h>
#include <stdbool.h>

static bool command_contains_unsafe_token(const char* command) {
    if (!command) return true;
    if (strstr(command, "../")) return true;
    if (strstr(command, "$(")) return true;
    if (strstr(command, "${")) return true;
    if (strchr(command, '`')) return true;
    size_t cmd_len = strlen(command);
    for (size_t i = 0; i < cmd_len; i++) {
        if (command[i] == '$' && i + 1 < cmd_len &&
            (command[i+1] == '(' || command[i+1] == '{' ||
             (command[i+1] >= 'A' && command[i+1] <= 'Z') ||
             (command[i+1] >= 'a' && command[i+1] <= 'z') ||
             command[i+1] == '_')) {
            return true;
        }
    }
    return false;
}

TEST(unsafe_null_command) {
    ASSERT_TRUE(command_contains_unsafe_token(NULL));
}

TEST(unsafe_dir_traversal) {
    ASSERT_TRUE(command_contains_unsafe_token("cat ../etc/passwd"));
    ASSERT_TRUE(command_contains_unsafe_token("ls ../../tmp"));
}

TEST(unsafe_command_substitution) {
    ASSERT_TRUE(command_contains_unsafe_token("echo $(whoami)"));
    ASSERT_TRUE(command_contains_unsafe_token("echo `whoami`"));
}

TEST(unsafe_env_variable) {
    ASSERT_TRUE(command_contains_unsafe_token("echo ${HOME}"));
    ASSERT_TRUE(command_contains_unsafe_token("echo $HOME"));
    ASSERT_TRUE(command_contains_unsafe_token("echo $API_KEY"));
}

TEST(safe_simple_command) {
    ASSERT_FALSE(command_contains_unsafe_token("ls -la"));
    ASSERT_FALSE(command_contains_unsafe_token("cat README.md"));
    ASSERT_FALSE(command_contains_unsafe_token("python3 script.py"));
}

TEST(safe_pipe) {
    ASSERT_FALSE(command_contains_unsafe_token("ls | grep test"));
    ASSERT_FALSE(command_contains_unsafe_token("cat file | head -20"));
    ASSERT_FALSE(command_contains_unsafe_token("ps aux | grep python"));
}

TEST(safe_chain) {
    ASSERT_FALSE(command_contains_unsafe_token("cd src && make"));
    ASSERT_FALSE(command_contains_unsafe_token("mkdir build || echo exists"));
    ASSERT_FALSE(command_contains_unsafe_token("cd dir; ls -la"));
}

TEST(safe_redirect) {
    ASSERT_FALSE(command_contains_unsafe_token("echo hello > output.txt"));
    ASSERT_FALSE(command_contains_unsafe_token("sort < input.txt"));
    ASSERT_FALSE(command_contains_unsafe_token("cat file >> log.txt"));
}

TEST(safe_absolute_path) {
    ASSERT_FALSE(command_contains_unsafe_token("/usr/bin/python3 script.py"));
    ASSERT_FALSE(command_contains_unsafe_token("/bin/ls -la"));
}

TEST(safe_home_dir) {
    ASSERT_FALSE(command_contains_unsafe_token("ls ~/Documents"));
    ASSERT_FALSE(command_contains_unsafe_token("cat ~/.bashrc"));
}

TEST(safe_background) {
    ASSERT_FALSE(command_contains_unsafe_token("sleep 10 &"));
}

TEST(safe_newlines) {
    ASSERT_FALSE(command_contains_unsafe_token("echo hello\necho world"));
}

TEST(safe_comments) {
    ASSERT_FALSE(command_contains_unsafe_token("echo test # this is a comment"));
}

TEST(safe_complex_commands) {
    ASSERT_FALSE(command_contains_unsafe_token("find . -name '*.c' | xargs grep TODO | head -20"));
    ASSERT_FALSE(command_contains_unsafe_token("cd src && make && ./run_tests"));
    ASSERT_FALSE(command_contains_unsafe_token("curl -s https://api.example.com/data | jq '.result'"));
}

TEST(unsafe_mixed) {
    ASSERT_TRUE(command_contains_unsafe_token("ls | grep $(cat secret)"));
    ASSERT_TRUE(command_contains_unsafe_token("cd ../ && ls"));
}

TEST(safe_empty_command) {
    ASSERT_FALSE(command_contains_unsafe_token(""));
}

TEST_SUITE(exec_security) {
    BEGIN_SUITE(exec_security);
    RUN_TEST(unsafe_null_command);
    RUN_TEST(unsafe_dir_traversal);
    RUN_TEST(unsafe_command_substitution);
    RUN_TEST(unsafe_env_variable);
    RUN_TEST(safe_simple_command);
    RUN_TEST(safe_pipe);
    RUN_TEST(safe_chain);
    RUN_TEST(safe_redirect);
    RUN_TEST(safe_absolute_path);
    RUN_TEST(safe_home_dir);
    RUN_TEST(safe_background);
    RUN_TEST(safe_newlines);
    RUN_TEST(safe_comments);
    RUN_TEST(safe_complex_commands);
    RUN_TEST(unsafe_mixed);
    RUN_TEST(safe_empty_command);
    END_SUITE();
}
