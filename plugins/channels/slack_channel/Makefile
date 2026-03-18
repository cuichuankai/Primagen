# Slack Channel Plugin Makefile

PLUGIN_NAME = slack_channel
PLUGIN_SRC = slack_channel.c
PLUGIN_SO = $(PLUGIN_NAME).so

PRIMAGEN_ROOT = ../../..
BUILD_DIR = $(PRIMAGEN_ROOT)/build

INCLUDES = -I$(PRIMAGEN_ROOT)/src/include \
           -I$(PRIMAGEN_ROOT)/src \
           -I$(PRIMAGEN_ROOT)/src/vendor

CC = gcc
CFLAGS = -fPIC -Wall -Wextra -std=c99 -g $(INCLUDES)

LDFLAGS = -shared -ldl -pthread
ifeq ($(shell uname -s),Darwin)
    LDFLAGS += -undefined dynamic_lookup
endif

all: $(BUILD_DIR) $(PLUGIN_SO)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(PLUGIN_SO): $(PLUGIN_SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	@echo "Built $(PLUGIN_SO)"

install: $(PLUGIN_SO)
	@mkdir -p $(BUILD_DIR)/.primagen/plugins/external
	cp $(PLUGIN_SO) $(BUILD_DIR)/.primagen/plugins/external/
	@echo "Installed $(PLUGIN_SO) to build/.primagen/plugins/external/"

clean:
	rm -f $(PLUGIN_SO)

rebuild: clean all

.PHONY: all install clean rebuild
