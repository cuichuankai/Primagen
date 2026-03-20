# =============================================================================
# Primagen Plugin Common Makefile
# Include this file from your plugin Makefile after setting:
#   PLUGIN_TYPE - channels, tools, or commands
#   PLUGIN_NAME - Your plugin name
#   PLUGIN_SRC - Source files
#   PRIMAGEN_ROOT - Path to primagen root (default: ../../..)
# =============================================================================
#
# Output structure (all platforms in same directory):
#   build/.primagen/plugins/<type>/<plugin>.so
#
# When building for multiple platforms, the last build wins.
# For cross-platform builds, use separate BUILD_DIR values.
# =============================================================================

# Set defaults
PRIMAGEN_ROOT ?= ../../..
BUILD_DIR ?= $(PRIMAGEN_ROOT)/build
PLUGIN_OUTPUT_DIR = $(BUILD_DIR)/.primagen/plugins/$(PLUGIN_TYPE)
RUNTIME_OUTPUT_DIR = $(PRIMAGEN_ROOT)/.primagen/plugins/$(PLUGIN_TYPE)

# Detect host OS
HOST_OS := $(shell uname -s)

# Platform-specific output name (all platforms output to same .so file)
PLUGIN_SO = $(PLUGIN_NAME).so

# =============================================================================
# Android NDK support
# =============================================================================

ifeq ($(ANDROID_NDK),)
    ifneq ($(wildcard /usr/local/android-ndk),)
        ANDROID_NDK = /usr/local/android-ndk
    else ifneq ($(wildcard $(HOME)/android-ndk),)
        ANDROID_NDK = $(HOME)/android-ndk
    else ifneq ($(wildcard $(HOME)/Library/Android/sdk/ndk/*),)
        ANDROID_NDK = $(wildcard $(HOME)/Library/Android/sdk/ndk/*)
    endif
endif

ANDROID_API ?= 21
ANDROID_ARCH ?= arm64
ANDROID_ABI ?= arm64-v8a

ifneq ($(ANDROID_NDK),)
    ANDROID_TOOLCHAIN_BASE = $(wildcard $(ANDROID_NDK)/toolchains/llvm/prebuilt/*)/bin
    ifeq ($(ANDROID_ARCH),arm64)
        ANDROID_CC = $(ANDROID_TOOLCHAIN_BASE)/aarch64-linux-android$(ANDROID_API)-clang
    else ifeq ($(ANDROID_ARCH),arm)
        ANDROID_CC = $(ANDROID_TOOLCHAIN_BASE)/armv7a-linux-androideabi$(ANDROID_API)-clang
    else ifeq ($(ANDROID_ARCH),x86_64)
        ANDROID_CC = $(ANDROID_TOOLCHAIN_BASE)/x86_64-linux-android$(ANDROID_API)-clang
    else ifeq ($(ANDROID_ARCH),x86)
        ANDROID_CC = $(ANDROID_TOOLCHAIN_BASE)/i686-linux-android$(ANDROID_API)-clang
    endif

    ANDROID_CFLAGS = -fPIC -Wall -Wextra -std=c99 -Os
    ANDROID_CFLAGS += -I$(PRIMAGEN_ROOT)/src/include -I$(PRIMAGEN_ROOT)/src -I$(PRIMAGEN_ROOT)/src/vendor

    ANDROID_LDFLAGS = -shared -ldl -pthread
else
    ANDROID_CC =
    ANDROID_CFLAGS =
    ANDROID_LDFLAGS =
endif

# =============================================================================
# Native Compiler flags
# =============================================================================

CC = gcc
CFLAGS = -fPIC -Wall -Wextra -std=c99 -g
CFLAGS += -I$(PRIMAGEN_ROOT)/src/include
CFLAGS += -I$(PRIMAGEN_ROOT)/src
CFLAGS += -I$(PRIMAGEN_ROOT)/src/vendor

LDFLAGS = -shared -ldl -pthread
ifeq ($(HOST_OS),Darwin)
    LDFLAGS += -undefined dynamic_lookup
endif

# =============================================================================
# Targets
# =============================================================================

.PHONY: all native android install clean rebuild help check-ndk

all: native

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# Native build
native: $(BUILD_DIR) $(PLUGIN_SO)
	@mkdir -p $(PLUGIN_OUTPUT_DIR)
	@cp $(PLUGIN_SO) $(PLUGIN_OUTPUT_DIR)/
	@echo "Built $(PLUGIN_SO) for $(HOST_OS) -> $(PLUGIN_OUTPUT_DIR)/"

# Android build (overwrites native .so)
android: check-ndk
	@$(MAKE) --no-print-directory TARGET_PLATFORM=android $(PLUGIN_SO)
	@mkdir -p $(PLUGIN_OUTPUT_DIR)
	@cp $(PLUGIN_SO) $(PLUGIN_OUTPUT_DIR)/
	@echo "Built $(PLUGIN_SO) for Android ($(ANDROID_ABI)) -> $(PLUGIN_OUTPUT_DIR)/"

check-ndk:
ifeq ($(ANDROID_CC),)
	@echo "Error: Android NDK not found. Set ANDROID_NDK environment variable."
	@echo "Common locations: /usr/local/android-ndk, $$HOME/android-ndk"
	@exit 1
else
	@echo "Using Android NDK: $(ANDROID_NDK)"
endif

# Build rule - use Android compiler if TARGET_PLATFORM=android
$(PLUGIN_SO): $(PLUGIN_SRC)
ifeq ($(TARGET_PLATFORM),android)
	$(ANDROID_CC) $(ANDROID_CFLAGS) $(ANDROID_LDFLAGS) -o $@ $(PLUGIN_SRC)
else
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PLUGIN_SRC)
endif

# Install to runtime directory
install: $(PLUGIN_SO)
	@mkdir -p $(RUNTIME_OUTPUT_DIR)
	@cp $(PLUGIN_SO) $(RUNTIME_OUTPUT_DIR)/
	@echo "Installed $(PLUGIN_SO) to $(RUNTIME_OUTPUT_DIR)/"

clean:
	rm -f $(PLUGIN_SO)

rebuild: clean all

help:
	@echo "Primagen Plugin: $(PLUGIN_NAME) ($(PLUGIN_TYPE))"
	@echo ""
	@echo "Targets:"
	@echo "  all/native     - Build for native platform (default)"
	@echo "  android        - Build for Android (arm64-v8a, API 21)"
	@echo "  install        - Install to runtime directory"
	@echo "  clean          - Remove built artifacts"
	@echo "  rebuild        - Clean and rebuild"
	@echo ""
	@echo "Variables:"
	@echo "  ANDROID_NDK    - Path to Android NDK"
	@echo "  ANDROID_API    - Android API level (default: 21)"
	@echo "  ANDROID_ARCH   - arm, arm64, x86, x86_64 (default: arm64)"
	@echo "  ANDROID_ABI    - armeabi-v7a, arm64-v8a, x86, x86_64 (default: arm64-v8a)"
	@echo ""
	@echo "Examples:"
	@echo "  make                                    # Native build"
	@echo "  make ANDROID_NDK=/path/to/ndk android   # Android build"
	@echo "  make install                            # Install"
