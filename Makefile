CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -pthread -I src/include -I src
OBJDIR = build
SRCDIR = src

.PHONY: all clean

all: $(OBJDIR)/primagen

OBJ = $(OBJDIR)/common/common.o $(OBJDIR)/common/message.o $(OBJDIR)/tools/tool.o $(OBJDIR)/session/session.o $(OBJDIR)/memory/memory.o $(OBJDIR)/context/context_builder.o $(OBJDIR)/bus/message_bus.o $(OBJDIR)/agent/agent_loop.o $(OBJDIR)/providers/llm_provider.o $(OBJDIR)/main.o

$(OBJDIR)/primagen: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

$(OBJDIR)/common/common.o: $(SRCDIR)/common/common.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/common/message.o: $(SRCDIR)/common/message.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/tools/tool.o: $(SRCDIR)/tools/tool.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/session/session.o: $(SRCDIR)/session/session.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/memory/memory.o: $(SRCDIR)/memory/memory.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/context/context_builder.o: $(SRCDIR)/context/context_builder.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/bus/message_bus.o: $(SRCDIR)/bus/message_bus.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/agent/agent_loop.o: $(SRCDIR)/agent/agent_loop.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/providers/llm_provider.o: $(SRCDIR)/providers/llm_provider.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/main.o: $(SRCDIR)/main.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR) $(OBJDIR)/common $(OBJDIR)/tools $(OBJDIR)/session $(OBJDIR)/memory $(OBJDIR)/context $(OBJDIR)/bus $(OBJDIR)/agent $(OBJDIR)/providers

clean:
	rm -rf $(OBJDIR)