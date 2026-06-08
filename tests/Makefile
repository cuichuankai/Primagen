CC = gcc

BASE_CFLAGS = -Wall -Wextra -Wno-unused-function -std=c99 -pthread -g -O0 \
              -I ../src/include -I ../src -I ../src/vendor \
              -DMG_TLS=MG_TLS_BUILTIN -DMG_ENABLE_LINES=1 -DMG_ENABLE_IPV6=1 \
              -DMG_ENABLE_SSI=1 -DMG_UECC_SUPPORTS_secp256r1=1 -DMG_ENABLE_CHACHA20=0 \
              -DMG_ENABLE_LOG=0 -DTEST_MODE

LDFLAGS = -ldl -lpthread

BUILD_DIR = ../build

COMMON_OBJS = $(BUILD_DIR)/common/common.o \
              $(BUILD_DIR)/common/message.o \
              $(BUILD_DIR)/common/logger.o \
              $(BUILD_DIR)/common/utils.o \
              $(BUILD_DIR)/vendor/cJSON/cJSON.o \
              $(BUILD_DIR)/vendor/mongoose/mongoose.o \
              $(BUILD_DIR)/config/config.o \
              $(BUILD_DIR)/providers/llm_provider.o \
              $(BUILD_DIR)/providers/openai_provider.o

TESTS = test_llm_provider_registry test_config_provider test_ft_integration test_runtime_fixes

.PHONY: all clean

all: dirs $(TESTS)

dirs:
	@mkdir -p $(BUILD_DIR)

test_llm_provider_registry: test_llm_provider_registry.c $(COMMON_OBJS)
	$(CC) $(BASE_CFLAGS) -o $@ test_llm_provider_registry.c $(COMMON_OBJS) $(LDFLAGS)

test_config_provider: test_config_provider.c $(BUILD_DIR)/config/config.o \
                      $(BUILD_DIR)/vendor/cJSON/cJSON.o $(BUILD_DIR)/vendor/mongoose/mongoose.o \
                      $(BUILD_DIR)/common/common.o $(BUILD_DIR)/common/logger.o \
                      $(BUILD_DIR)/common/utils.o $(BUILD_DIR)/common/message.o
	$(CC) $(BASE_CFLAGS) -o $@ test_config_provider.c $(BUILD_DIR)/config/config.o \
	      $(BUILD_DIR)/vendor/cJSON/cJSON.o $(BUILD_DIR)/vendor/mongoose/mongoose.o \
	      $(BUILD_DIR)/common/common.o $(BUILD_DIR)/common/logger.o \
	      $(BUILD_DIR)/common/utils.o $(BUILD_DIR)/common/message.o $(LDFLAGS)

test_ft_integration: test_ft_integration.c $(COMMON_OBJS)
	$(CC) $(BASE_CFLAGS) -o $@ test_ft_integration.c $(COMMON_OBJS) $(LDFLAGS)

test_runtime_fixes: test_runtime_fixes.c $(COMMON_OBJS) \
                    $(BUILD_DIR)/session/session.o $(BUILD_DIR)/tools/tools_impl.o \
                    $(BUILD_DIR)/tools/tool_validation.o $(BUILD_DIR)/tools/tool.o \
                    $(BUILD_DIR)/memory/memory.o $(BUILD_DIR)/skills/skills.o \
                    $(BUILD_DIR)/cron/cron.o $(BUILD_DIR)/subagent/subagent.o \
                    $(BUILD_DIR)/plugin/plugin_manager.o $(BUILD_DIR)/bus/message_bus.o
	$(CC) $(BASE_CFLAGS) -o $@ test_runtime_fixes.c $(COMMON_OBJS) \
	      $(BUILD_DIR)/session/session.o $(BUILD_DIR)/tools/tools_impl.o \
	      $(BUILD_DIR)/tools/tool_validation.o $(BUILD_DIR)/tools/tool.o \
	      $(BUILD_DIR)/memory/memory.o $(BUILD_DIR)/skills/skills.o \
	      $(BUILD_DIR)/cron/cron.o $(BUILD_DIR)/subagent/subagent.o \
	      $(BUILD_DIR)/plugin/plugin_manager.o $(BUILD_DIR)/bus/message_bus.o $(LDFLAGS)

clean:
	rm -f $(TESTS)
