# Hello Command Plugin Makefile
# Builds a standalone .so plugin for Primagen

PLUGIN_NAME = hello_command
PLUGIN_SRC = hello_command.c
PLUGIN_SO = $(PLUGIN_NAME).so

# Primagen source root (for headers and build objects)
PRIMAGEN_ROOT = ../../..
BUILD_DIR = $(PRIMAGEN_ROOT)/build

# Include paths
INCLUDES = -I$(PRIMAGEN_ROOT)/src/include \
           -I$(PRIMAGEN_ROOT)/src \
           -I$(PRIMAGEN_ROOT)/src/vendor

# Compiler flags
CC = gcc
CFLAGS = -fPIC -Wall -Wextra -std=c99 -g $(INCLUDES)

# Linker flags - use -undefined dynamic_lookup for macOS
LDFLAGS = -shared -ldl
ifeq ($(shell uname -s),Darwin)
    LDFLAGS += -undefined dynamic_lookup
endif

# Default target
all: $(BUILD_DIR) $(PLUGIN_SO)

# Ensure build directory exists
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# Build the plugin
$(PLUGIN_SO): $(PLUGIN_SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	@echo "Built $(PLUGIN_SO)"

# Install to Primagen plugins directory
install: $(PLUGIN_SO)
	@mkdir -p $(BUILD_DIR)/.primagen/plugins/external
	cp $(PLUGIN_SO) $(BUILD_DIR)/.primagen/plugins/external/
	@echo "Installed $(PLUGIN_SO) to build/.primagen/plugins/external/"

# Clean
clean:
	rm -f $(PLUGIN_SO)

# Rebuild
rebuild: clean all

.PHONY: all install clean rebuild
