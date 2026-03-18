# Primagen Plugins Root Makefile
# Builds all plugins in subdirectories

.PHONY: all clean install rebuild tools channels commands

# Default target - build all plugins
all: tools channels commands
	@echo "All plugins built successfully!"

# Build tool plugins
tools:
	@echo "Building tool plugins..."
	@cd tools/echo_tool && $(MAKE) --no-print-directory

# Build channel plugins
channels:
	@echo "Building channel plugins..."
	@cd channels/stdout_channel && $(MAKE) --no-print-directory
	@cd channels/telegram_channel && $(MAKE) --no-print-directory
	@cd channels/email_channel && $(MAKE) --no-print-directory
	@cd channels/discord_channel && $(MAKE) --no-print-directory
	@cd channels/slack_channel && $(MAKE) --no-print-directory
	@cd channels/dingtalk_channel && $(MAKE) --no-print-directory

# Build command plugins
commands:
	@echo "Building command plugins..."
	@cd commands/hello_command && $(MAKE) --no-print-directory

# Install all plugins
install: all
	@echo "Installing all plugins..."
	@cd tools/echo_tool && $(MAKE) --no-print-directory install
	@cd channels/stdout_channel && $(MAKE) --no-print-directory install
	@cd channels/telegram_channel && $(MAKE) --no-print-directory install
	@cd channels/email_channel && $(MAKE) --no-print-directory install
	@cd channels/discord_channel && $(MAKE) --no-print-directory install
	@cd channels/slack_channel && $(MAKE) --no-print-directory install
	@cd channels/dingtalk_channel && $(MAKE) --no-print-directory install
	@cd commands/hello_command && $(MAKE) --no-print-directory install
	@echo "All plugins installed to build/.primagen/plugins/"

# Clean all plugins
clean:
	@echo "Cleaning all plugins..."
	@cd tools/echo_tool && $(MAKE) --no-print-directory clean
	@cd channels/stdout_channel && $(MAKE) --no-print-directory clean
	@cd channels/telegram_channel && $(MAKE) --no-print-directory clean
	@cd channels/email_channel && $(MAKE) --no-print-directory clean
	@cd channels/discord_channel && $(MAKE) --no-print-directory clean
	@cd channels/slack_channel && $(MAKE) --no-print-directory clean
	@cd channels/dingtalk_channel && $(MAKE) --no-print-directory clean
	@cd commands/hello_command && $(MAKE) --no-print-directory clean
	@echo "All plugins cleaned."

# Rebuild all plugins
rebuild: clean all

# Help
help:
	@echo "Primagen Plugins Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build all plugins (default)"
	@echo "  tools    - Build tool plugins only"
	@echo "  channels - Build channel plugins only"
	@echo "  commands - Build command plugins only"
	@echo "  install  - Build and install all plugins"
	@echo "  clean    - Remove built plugins"
	@echo "  rebuild  - Clean and rebuild all plugins"
	@echo "  help     - Show this help message"
	@echo ""
	@echo "Demo Plugins:"
	@echo "  tools/echo_tool       - Tool that echoes user messages"
	@echo "  channels/stdout_channel - Channel that outputs to stdout"
	@echo "  channels/telegram_channel - Telegram channel support"
	@echo "  channels/email_channel - Email channel support"
	@echo "  channels/discord_channel - Discord channel support"
	@echo "  channels/slack_channel - Slack channel support"
	@echo "  channels/dingtalk_channel - DingTalk channel support"
	@echo "  commands/hello_command - Adds /hello slash command"
