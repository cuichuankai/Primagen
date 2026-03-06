CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -pthread -I src/include -I src -I src/vendor
LDFLAGS = -lcurl

OBJDIR = build
SRCDIR = src

.PHONY: all clean test test_tools

all: $(OBJDIR)/primagen

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

$(OBJDIR)/main.o: $(SRCDIR)/main.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Test targets
test: test_agent_loop test_tools

test_agent_loop: $(OBJDIR)/test_agent_loop
	./$(OBJDIR)/test_agent_loop

test_tools: $(OBJDIR)/test_tools
	./$(OBJDIR)/test_tools

$(OBJDIR)/test_agent_loop: tests/test_agent_loop.c $(filter-out $(OBJDIR)/main.o $(OBJDIR)/channels/console.o $(OBJDIR)/channels/telegram.o $(OBJDIR)/cli/commands.o, $(OBJ))
	$(CC) $(CFLAGS) -o $@ $< $(filter-out $(OBJDIR)/main.o $(OBJDIR)/channels/console.o $(OBJDIR)/channels/telegram.o $(OBJDIR)/cli/commands.o, $(OBJ)) $(LDFLAGS)

$(OBJDIR)/test_tools: tests/test_tools.c $(filter-out $(OBJDIR)/main.o $(OBJDIR)/channels/console.o $(OBJDIR)/channels/telegram.o $(OBJDIR)/cli/commands.o, $(OBJ))
	$(CC) $(CFLAGS) -o $@ $< $(filter-out $(OBJDIR)/main.o $(OBJDIR)/channels/console.o $(OBJDIR)/channels/telegram.o $(OBJDIR)/cli/commands.o, $(OBJ)) $(LDFLAGS)

clean:
	rm -rf $(OBJDIR)
