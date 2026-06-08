#include "test_framework.h"

int g_tests_run = 0;
int g_tests_passed = 0;
int g_tests_failed = 0;
int g_assertions = 0;

extern void run_message_bus(void);
extern void run_session(void);
extern void run_tool_registry(void);
extern void run_utils(void);
extern void run_config(void);
extern void run_slash_command(void);
extern void run_exec_security(void);

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    
    printf("============================================\n");
    printf("  Primagen Unit Test Suite\n");
    printf("============================================\n");
    
    run_message_bus();
    run_session();
    run_tool_registry();
    run_utils();
    run_config();
    run_slash_command();
    run_exec_security();
    
    print_test_summary();
    return test_exit_code();
}
