# Default target
TARGET ?= macos
BUILD_TYPE ?= debug

# Android Configuration
ANDROID_API ?= 24
ANDROID_ARCH ?= aarch64
ANDROID_HOST ?= darwin-x86_64

ifeq ($(TARGET),android)
    # Check if ANDROID_NDK_HOME is set
    ifndef ANDROID_NDK
        $(error ANDROID_NDK is not set)
    endif

    TOOLCHAIN = $(ANDROID_NDK)/toolchains/llvm/prebuilt/$(ANDROID_HOST)
    # Correct path for clang in newer NDKs
    CC = $(TOOLCHAIN)/bin/aarch64-linux-android$(ANDROID_API)-clang

    # Android specific flags (release)
    CFLAGS = -Wall -Wextra -std=c99 -pthread -O2 -DNDEBUG -DMG_ENABLE_LOG=0 -I src/include -I src -I src/vendor -DANDROID -DMG_TLS=MG_TLS_BUILTIN
    LDFLAGS = -llog -Wl,--export-dynamic
else
    CC = gcc
    BASE_CFLAGS = -Wall -Wextra -std=c99 -pthread -I src/include -I src -I src/vendor -DMG_TLS=MG_TLS_BUILTIN -DMG_ENABLE_LINES=1 -DMG_ENABLE_IPV6=1 -DMG_ENABLE_SSI=1 -DMG_UECC_SUPPORTS_secp256r1=1 -DMG_ENABLE_CHACHA20=0
    ifeq ($(BUILD_TYPE),release)
        CFLAGS = $(BASE_CFLAGS) -O2 -DNDEBUG -DMG_ENABLE_LOG=0
    else
        CFLAGS = $(BASE_CFLAGS) -g -O0 -DMG_ENABLE_LOG=0
    endif
    LDFLAGS = -ldl
endif

OBJDIR = build/
SRCDIR = src

.PHONY: all clean package dirs android test plugins

all: dirs plugins $(OBJDIR)/primagen

dirs:
	@mkdir -p $(OBJDIR)
	@mkdir -p $(OBJDIR)/.primagen/plugins

android:
	$(MAKE) TARGET=android all

OBJ = $(OBJDIR)/common/common.o \
      $(OBJDIR)/common/message.o \
      $(OBJDIR)/common/logger.o \
      $(OBJDIR)/common/utils.o \
      $(OBJDIR)/tools/tool.o \
      $(OBJDIR)/tools/tools_impl.o \
      $(OBJDIR)/tools/tool_validation.o \
      $(OBJDIR)/tools/tool_executor.o \
      $(OBJDIR)/session/session.o \
      $(OBJDIR)/memory/memory.o \
      $(OBJDIR)/context/context_builder.o \
      $(OBJDIR)/bus/message_bus.o \
      $(OBJDIR)/agent/agent_loop.o \
      $(OBJDIR)/agent/command_dispatcher.o \
      $(OBJDIR)/agent/tool_router.o \
      $(OBJDIR)/agent/session_orchestrator.o \
      $(OBJDIR)/providers/llm_provider.o \
      $(OBJDIR)/providers/openai_provider.o \
      $(OBJDIR)/subagent/subagent.o \
      $(OBJDIR)/cron/cron.o \
      $(OBJDIR)/heartbeat/heartbeat.o \
      $(OBJDIR)/skills/skills.o \
      $(OBJDIR)/config/config.o \
      $(OBJDIR)/plugin/plugin_manager.o \
      $(OBJDIR)/channels/console.o \
      $(OBJDIR)/cli/commands.o \
      $(OBJDIR)/vendor/cJSON/cJSON.o \
      $(OBJDIR)/vendor/mongoose/mongoose.o \
      $(OBJDIR)/main.o

$(OBJDIR)/primagen: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# External plugins (shared libraries - for dynamic loading)
PLUGIN_CFLAGS = -fPIC -shared -Wall -Wextra -std=c99 -I src/include -I src -I src/vendor

plugins: dirs
	@echo "External plugins can be loaded at runtime from $(OBJDIR)/.primagen/plugins/external/"

# Build a sample external plugin (requires linking against main binary symbols)
# For now, external plugins are loaded via dlopen and get symbols dynamically

package:
	@echo "Creating self-extracting installer package..."
	cp install.sh $(OBJDIR)/primagen_install.sh
	mkdir -p $(OBJDIR)/.primagen
	mkdir -p $(OBJDIR)/.primagen/plugins/channels
	cp -r skills $(OBJDIR)/.primagen/
	cd $(OBJDIR);tar -czf - ./primagen ./.primagen >> primagen_install.sh;cd -
	chmod +x $(OBJDIR)/primagen_install.sh
	@echo "Package created: $(OBJDIR)/primagen_install.sh"

clean:
	rm -rf build
