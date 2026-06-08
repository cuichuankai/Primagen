#include "test_framework.h"
#include "../src/include/config.h"
#include "../src/include/common.h"

TEST(config_create_default) {
    Config* cfg = config_create();
    ASSERT_NOT_NULL(cfg);
    ASSERT_NOT_NULL(cfg->agent.model);
    ASSERT_TRUE(cfg->agent.max_tokens > 0);
    ASSERT_TRUE(cfg->agent.temperature >= 0.0 && cfg->agent.temperature <= 2.0);
    config_destroy(cfg);
}

TEST(config_validate_valid) {
    Config* cfg = config_create();
    Error err = config_validate(cfg);
    ASSERT_EQ_INT(ERR_NONE, err.code);
    config_destroy(cfg);
}

TEST(config_validate_null) {
    Error err = config_validate(NULL);
    ASSERT_TRUE(err.code != ERR_NONE);
}

TEST(config_validate_empty_model) {
    Config* cfg = config_create();
    free(cfg->agent.model);
    cfg->agent.model = strdup("");
    Error err = config_validate(cfg);
    ASSERT_TRUE(err.code != ERR_NONE);
    config_destroy(cfg);
}

TEST(config_validate_negative_max_tokens) {
    Config* cfg = config_create();
    cfg->agent.max_tokens = -1;
    Error err = config_validate(cfg);
    ASSERT_TRUE(err.code != ERR_NONE);
    config_destroy(cfg);
}

TEST(config_validate_zero_max_tokens) {
    Config* cfg = config_create();
    cfg->agent.max_tokens = 0;
    Error err = config_validate(cfg);
    ASSERT_TRUE(err.code != ERR_NONE);
    config_destroy(cfg);
}

TEST(config_validate_temperature_too_high) {
    Config* cfg = config_create();
    cfg->agent.temperature = 3.0;
    Error err = config_validate(cfg);
    ASSERT_TRUE(err.code != ERR_NONE);
    config_destroy(cfg);
}

TEST(config_validate_temperature_negative) {
    Config* cfg = config_create();
    cfg->agent.temperature = -0.5;
    Error err = config_validate(cfg);
    ASSERT_TRUE(err.code != ERR_NONE);
    config_destroy(cfg);
}

TEST(config_validate_temperature_boundary) {
    Config* cfg = config_create();
    cfg->agent.temperature = 0.0;
    Error err = config_validate(cfg);
    ASSERT_EQ_INT(ERR_NONE, err.code);
    cfg->agent.temperature = 2.0;
    err = config_validate(cfg);
    ASSERT_EQ_INT(ERR_NONE, err.code);
    config_destroy(cfg);
}

TEST(config_validate_empty_log_level) {
    Config* cfg = config_create();
    free(cfg->log.level);
    cfg->log.level = strdup("");
    Error err = config_validate(cfg);
    ASSERT_TRUE(err.code != ERR_NONE);
    config_destroy(cfg);
}

TEST(config_save_load_roundtrip) {
    Config* cfg = config_create();
    const char* path = "/tmp/test_config_roundtrip.json";
    bool saved = config_save_to_file(cfg, path);
    ASSERT_TRUE(saved);
    
    Config* cfg2 = config_create();
    bool loaded = config_load_from_file(cfg2, path);
    ASSERT_TRUE(loaded);
    ASSERT_EQ_STR(cfg->agent.model, cfg2->agent.model);
    
    config_destroy(cfg);
    config_destroy(cfg2);
    remove(path);
}

TEST(config_load_nonexistent) {
    Config* cfg = config_create();
    bool loaded = config_load_from_file(cfg, "/tmp/no_such_config_file_12345.json");
    ASSERT_TRUE(loaded);
    config_destroy(cfg);
}

TEST_SUITE(config) {
    BEGIN_SUITE(config);
    RUN_TEST(config_create_default);
    RUN_TEST(config_validate_valid);
    RUN_TEST(config_validate_null);
    RUN_TEST(config_validate_empty_model);
    RUN_TEST(config_validate_negative_max_tokens);
    RUN_TEST(config_validate_zero_max_tokens);
    RUN_TEST(config_validate_temperature_too_high);
    RUN_TEST(config_validate_temperature_negative);
    RUN_TEST(config_validate_temperature_boundary);
    RUN_TEST(config_validate_empty_log_level);
    RUN_TEST(config_save_load_roundtrip);
    RUN_TEST(config_load_nonexistent);
    END_SUITE();
}
