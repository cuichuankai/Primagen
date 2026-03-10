# Default target
TARGET ?= macos

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
    
    # Android specific flags
    CFLAGS = -Wall -Wextra -std=c99 -pthread -g -I src/include -I src -I src/vendor -DANDROID -DMG_TLS=MG_TLS_BUILTIN
    LDFLAGS = -llog
else
    CC = gcc
    CFLAGS = -Wall -Wextra -std=c99 -pthread -g -I src/include -I src -I src/vendor -DMG_TLS=MG_TLS_BUILTIN -DMG_ENABLE_LINES=1 -DMG_ENABLE_IPV6=1 -DMG_ENABLE_SSI=1 -DMG_UECC_SUPPORTS_secp256r1=1 -DMG_ENABLE_CHACHA20=0
    LDFLAGS = 
endif

OBJDIR = build/
SRCDIR = src

.PHONY: all clean package dirs android

all: dirs $(OBJDIR)/primagen

dirs:
	@mkdir -p $(OBJDIR)

android:
	$(MAKE) TARGET=android all

OBJ = $(OBJDIR)/common/common.o \
      $(OBJDIR)/common/message.o \
      $(OBJDIR)/common/logger.o \
      $(OBJDIR)/tools/tool.o \
      $(OBJDIR)/tools/tools_impl.o \
      $(OBJDIR)/session/session.o \
      $(OBJDIR)/memory/memory.o \
      $(OBJDIR)/context/context_builder.o \
      $(OBJDIR)/bus/message_bus.o \
      $(OBJDIR)/agent/agent_loop.o \
      $(OBJDIR)/providers/llm_provider.o \
      $(OBJDIR)/subagent/subagent.o \
      $(OBJDIR)/cron/cron.o \
      $(OBJDIR)/heartbeat/heartbeat.o \
      $(OBJDIR)/skills/skills.o \
      $(OBJDIR)/config/config.o \
      $(OBJDIR)/channels/console.o \
      $(OBJDIR)/channels/telegram.o \
      $(OBJDIR)/channels/email.o \
      $(OBJDIR)/channels/discord.o \
      $(OBJDIR)/channels/slack.o \
      $(OBJDIR)/channels/dingtalk.o \
      $(OBJDIR)/channels/feishu.o \
      $(OBJDIR)/channels/feishu_ws.o \
      $(OBJDIR)/cli/commands.o \
      $(OBJDIR)/vendor/cJSON/cJSON.o \
      $(OBJDIR)/vendor/mongoose/mongoose.o \
      $(OBJDIR)/main.o

$(OBJDIR)/primagen: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

$(OBJDIR)/common/%.o: $(SRCDIR)/common/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/tools/%.o: $(SRCDIR)/tools/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/session/%.o: $(SRCDIR)/session/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/memory/%.o: $(SRCDIR)/memory/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/context/%.o: $(SRCDIR)/context/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/bus/%.o: $(SRCDIR)/bus/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/agent/%.o: $(SRCDIR)/agent/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/providers/%.o: $(SRCDIR)/providers/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/subagent/%.o: $(SRCDIR)/subagent/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/cron/%.o: $(SRCDIR)/cron/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/heartbeat/%.o: $(SRCDIR)/heartbeat/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/skills/%.o: $(SRCDIR)/skills/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/config/%.o: $(SRCDIR)/config/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/channels/%.o: $(SRCDIR)/channels/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/cli/%.o: $(SRCDIR)/cli/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/vendor/cJSON/%.o: $(SRCDIR)/vendor/cJSON/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/vendor/mongoose/%.o: $(SRCDIR)/vendor/mongoose/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/main.o: $(SRCDIR)/main.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@


package:
	@echo "Creating self-extracting installer package..."
	cp install.sh $(OBJDIR)/primagen_install.sh
	mkdir -p $(OBJDIR)/.primagen
	cp -r skills $(OBJDIR)/.primagen/
	cd $(OBJDIR);tar -czf - ./primagen ./.primagen >> primagen_install.sh;cd -
	chmod +x $(OBJDIR)/primagen_install.sh
	@echo "Package created: $(OBJDIR)/primagen_install.sh"

clean:
	rm -rf build
