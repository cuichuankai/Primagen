# =============================================================================
# Primagen Plugins Root Makefile
# Builds all plugins organized by type
# =============================================================================
#
# Directory structure after build:
#   build/.primagen/plugins/
#   ├── channels/     - Channel plugins (feishu, dingtalk, telegram, etc.)
#   ├── tools/        - Tool plugins (echo_tool, etc.)
#   └── commands/     - Command plugins (hello_command, etc.)
#
# All platforms (native/Android) output to the same directory.
# Building for a different platform overwrites the previous build.
#
# Usage:
#   make                        - Build all plugins for native platform
#   make android                - Build all plugins for Android
#   make channels               - Build channel plugins only
#   make tools                  - Build tool plugins only
#   make commands               - Build command plugins only
#   make install                - Build and install all plugins
#   make clean                  - Remove all built artifacts
#   make rebuild                - Clean and rebuild all
#
# Environment variables:
#   ANDROID_NDK - Path to Android NDK (for android target)
# =============================================================================

# Plugin directories
CHANNELS_DIR = channels
TOOLS_DIR = tools
COMMANDS_DIR = commands
LLM_DIR = llm

# Collect all plugin subdirectories
CHANNEL_PLUGINS := $(wildcard $(CHANNELS_DIR)/*)
TOOL_PLUGINS := $(wildcard $(TOOLS_DIR)/*)
COMMAND_PLUGINS := $(wildcard $(COMMANDS_DIR)/*)
LLM_PLUGINS := $(wildcard $(LLM_DIR)/*)

# Mark all phony targets
.PHONY: all clean install rebuild tools channels commands llm android help native install-native
.PHONY: $(CHANNEL_PLUGINS) $(TOOL_PLUGINS) $(COMMAND_PLUGINS) $(LLM_PLUGINS)

# Default target - build all plugins
all: native
	@echo ""
	@echo "=========================================="
	@echo "All plugins built successfully!"
	@echo "=========================================="

# =============================================================================
# Native builds
# =============================================================================

native: channels tools commands llm

channels: $(CHANNEL_PLUGINS)

tools: $(TOOL_PLUGINS)

commands: $(COMMAND_PLUGINS)

llm: $(LLM_PLUGINS)

# Build individual plugins (use subshell for cd)
$(CHANNEL_PLUGINS) $(TOOL_PLUGINS) $(COMMAND_PLUGINS) $(LLM_PLUGINS):
	@echo ""
	@echo "--- Building plugin: $@ ---"
	@(cd $@ && $(MAKE) --no-print-directory)

# =============================================================================
# Android builds (outputs to same directory, overwrites native)
# =============================================================================

android: check-android-ndk
	@echo "Building all plugins for Android..."
	@for plugin in $(CHANNEL_PLUGINS) $(TOOL_PLUGINS) $(COMMAND_PLUGINS) $(LLM_PLUGINS); do \
		echo "--- Building Android plugin: $$plugin ---"; \
		(cd $$plugin && $(MAKE) --no-print-directory android ANDROID_NDK=$(ANDROID_NDK)) || true; \
	done
	@echo ""
	@echo "=========================================="
	@echo "All Android plugins built successfully!"
	@echo "=========================================="

check-android-ndk:
ifeq ($(ANDROID_NDK),)
	@echo "Error: ANDROID_NDK not set or invalid path"
	@echo "Please set ANDROID_NDK environment variable:"
	@echo "  export ANDROID_NDK=/path/to/android-ndk"
	@exit 1
endif

# =============================================================================
# Installation
# =============================================================================

install: native
	@echo ""
	@echo "Installing plugins..."
	@for plugin in $(CHANNEL_PLUGINS) $(TOOL_PLUGINS) $(COMMAND_PLUGINS) $(LLM_PLUGINS); do \
		(cd $$plugin && $(MAKE) --no-print-directory install) || true; \
	done
	@echo ""
	@echo "All plugins installed to .primagen/plugins/"

# =============================================================================
# Cleanup
# =============================================================================

clean:
	@echo "Cleaning all plugins..."
	@for plugin in $(CHANNEL_PLUGINS) $(TOOL_PLUGINS) $(COMMAND_PLUGINS) $(LLM_PLUGINS); do \
		(cd $$plugin && $(MAKE) --no-print-directory clean) || true; \
	done
	@echo "All plugins cleaned."

rebuild: clean all

# =============================================================================
# Help
# =============================================================================

help:
	@echo "Primagen Plugins Build System"
	@echo "=============================="
	@echo ""
	@echo "Targets:"
	@echo "  all/native    - Build all plugins for native platform (default)"
	@echo "  android       - Build all plugins for Android (arm64-v8a)"
	@echo "  channels      - Build channel plugins only"
	@echo "  tools         - Build tool plugins only"
	@echo "  commands      - Build command plugins only"
	@echo "  llm           - Build LLM provider plugins"
	@echo "  install       - Build and install all plugins"
	@echo "  clean         - Remove all built artifacts"
	@echo "  rebuild       - Clean and rebuild all plugins"
	@echo "  help          - Show this help message"
	@echo ""
	@echo "Environment Variables:"
	@echo "  ANDROID_NDK   - Path to Android NDK (required for android target)"
	@echo ""
	@echo "Output Structure:"
	@echo "  build/.primagen/plugins/"
	@echo "  ├── channels/   - Channel plugin .so files"
	@echo "  ├── tools/      - Tool plugin .so files"
	@echo "  ├── commands/   - Command plugin .so files"
	@echo "  └── llm/        - LLM provider plugin .so files"
	@echo ""
	@echo "Examples:"
	@echo "  make                                    # Build all native plugins"
	@echo "  make channels                           # Build channel plugins only"
	@echo "  make ANDROID_NDK=/path/to/ndk android   # Build all Android plugins"
	@echo "  make install                            # Build and install"
	@echo "  make clean && make                      # Rebuild all"
