PRIMAGEN_ROOT = ../../..
PLUGIN_TYPE = channels
PLUGIN_NAME = channel_common
PLUGIN_SRC = channel_common.c

CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -fPIC -I. -I$(PRIMAGEN_ROOT)/src/vendor/mongoose -I$(PRIMAGEN_ROOT)/src/vendor/cJSON

BUILD_DIR = $(PRIMAGEN_ROOT)/build/.primagen/plugins/$(PLUGIN_TYPE)

all: $(BUILD_DIR)/lib$(PLUGIN_NAME).a

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/channel_common.o: channel_common.c channel_common.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/lib$(PLUGIN_NAME).a: $(BUILD_DIR)/channel_common.o | $(BUILD_DIR)
	ar rcs $@ $<

clean:
	rm -f $(BUILD_DIR)/channel_common.o $(BUILD_DIR)/lib$(PLUGIN_NAME).a

.PHONY: all clean
