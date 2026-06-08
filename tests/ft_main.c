#include "test_framework.h"

int g_tests_run = 0;
int g_tests_passed = 0;
int g_tests_failed = 0;
int g_assertions = 0;

extern void run_integration(void);
extern void run_concurrent(void);

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    
    printf("============================================\n");
    printf("  Primagen Functional Test Suite\n");
    printf("============================================\n");
    
    run_integration();
    run_concurrent();
    
    print_test_summary();
    return test_exit_code();
}
