# DingTalk Channel Plugin Makefile

PLUGIN_NAME = dingtalk_channel
PLUGIN_SRC = dingtalk_channel.c dingtalk_ws.c
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
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PLUGIN_SRC)
	@echo "Built $(PLUGIN_SO)"

install: $(PLUGIN_SO)
	@mkdir -p $(BUILD_DIR)/.primagen/plugins/channels
	cp $(PLUGIN_SO) $(BUILD_DIR)/.primagen/plugins/channels/
	@mkdir -p $(PRIMAGEN_ROOT)/.primagen/plugins/channels
	cp $(PLUGIN_SO) $(PRIMAGEN_ROOT)/.primagen/plugins/channels/
	@echo "Installed $(PLUGIN_SO) to build/.primagen/plugins/channels/ and .primagen/plugins/channels/"

clean:
	rm -f $(PLUGIN_SO)

rebuild: clean all

.PHONY: all install clean rebuild
