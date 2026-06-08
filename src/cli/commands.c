#include "../include/commands.h"
#include "../include/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Helper to check if file exists */
static bool file_exists(const char* path) {
    return access(path, F_OK) != -1;
}

/* Helper to create directory */
static void create_dir(const char* path) {
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        mkdir(path, 0755);
    }
}

/* Initialize configuration and workspace */
int cmd_onboard(const char* config_path, const char* workspace_path) {
    printf("Initializing primagen configuration and workspace...\n");

    /* 1. Create Config Directory */
    char config_dir[512];
    char* last_slash = strrchr(config_path, '/');
    if (last_slash) {
        size_t len = last_slash - config_path;
        strncpy(config_dir, config_path, len);
        config_dir[len] = '\0';
        create_dir(config_dir);
    }

    /* 2. Create/Check Config File */
    if (file_exists(config_path)) {
        printf("[yellow]Config already exists at %s[/yellow]\n", config_path);
        /* Simple logic: just warn, don't overwrite unless force (not implemented) */
    } else {
        Config* cfg = config_create();
        if (config_save_to_file(cfg, config_path)) {
            printf("[green]✓[/green] Created config at %s\n", config_path);
        } else {
            printf("[red]Failed to create config at %s[/red]\n", config_path);
        }
        config_destroy(cfg);
    }

    /* 3. Create Workspace */
    create_dir(workspace_path);
    printf("[green]✓[/green] Workspace ready at %s\n", workspace_path);

    /* 4. Create Templates */
    /* AGENTS.md */
    char agents_path[512];
    snprintf(agents_path, sizeof(agents_path), "%s/AGENTS.md", workspace_path);
    if (!file_exists(agents_path)) {
        FILE* fp = fopen(agents_path, "w");
        if (fp) {
            fprintf(fp, "# Primagen\n\n");
            fprintf(fp, "You are Primagen, a helpful AI assistant.\n\n");
            fprintf(fp, "## Guidelines\n\n");
            fprintf(fp, "- State intent before tool calls, but NEVER predict or claim results before receiving them.\n");
            fprintf(fp, "- Before modifying a file, read it first. Do not assume files or directories exist.\n");
            fprintf(fp, "- If a tool call fails, analyze the error before retrying with a different approach.\n");
            fprintf(fp, "- Ask for clarification when the request is ambiguous.\n\n");
            fprintf(fp, "## Tool Usage\n\n");
            fprintf(fp, "- You have access to various tools (cron, memory, exec, skill, etc.).\n");
            fprintf(fp, "- When user asks for something that matches a tool's purpose, CALL THAT TOOL.\n");
            fprintf(fp, "- Actually call the tool - don't just talk about it.\n\n");
            fprintf(fp, "## Cron / Reminder Instructions\n\n");
            fprintf(fp, "When the user asks you to set a reminder, schedule a task, or says anything like \"remind me in N minutes\", \"remind me every day at X\", you MUST call the `cron` tool. Text-only acknowledgment is NEVER sufficient.\n\n");
            fprintf(fp, "1. **Always Call cron Tool**: Saying \"I've set a reminder for you\" or \"OK, I'll remind you\" WITHOUT calling the `cron` tool means NO reminder will be sent. You MUST call the tool.\n");
            fprintf(fp, "2. **Convert Time Expressions**: Parse the user's time expression and convert to cron schedule format:\n");
            fprintf(fp, "   - \"N minutes/seconds later\" -> `@in N*60` or `@in N`\n");
            fprintf(fp, "   - \"Every day at X\" -> `0 X * * *`\n");
            fprintf(fp, "   - \"Every hour\" -> `@every 3600`\n");
            fprintf(fp, "3. **Cron Tool Examples**:\n");
            fprintf(fp, "   - User says \"remind me to drink water in 1 minute\" -> Call `cron(name=\"drink-water\", payload=\"Remind the user to drink water\", schedule=\"@in 60\")`\n");
            fprintf(fp, "   - User says \"remind me to have a meeting every day at 8am\" -> Call `cron(name=\"morning-meeting\", payload=\"Remind the user to have a meeting\", schedule=\"0 8 * * *\")`\n\n");
            fprintf(fp, "**CRITICAL**: Claiming you set a reminder WITHOUT calling the `cron` tool means the user will NEVER receive the reminder. You MUST call the tool.\n\n");
            fprintf(fp, "## Core Memory Instructions\n\n");
            fprintf(fp, "You represent a long-term companion. To maintain continuity across sessions, you MUST proactively manage your memory.\n\n");
            fprintf(fp, "1.  **Save Facts**: When the user provides important personal information (names, nicknames, relationships, preferences, project details), IMMEDIATELY use the `memory` tool to save it. Do NOT just acknowledge in text - you MUST call the `memory` tool.\n");
            fprintf(fp, "2.  **Consolidate History**: If a conversation covers important decisions or events, use the `memory` tool with `history_entry` to save a summary.\n");
            fprintf(fp, "3.  **Remember Requests**: When the user explicitly asks you to \"remember\", \"save\", \"note\" something, you MUST call the `memory` tool with `memory_update` to persist the fact. Text-only acknowledgment is NOT sufficient.\n");
            fprintf(fp, "4.  **Memory Tool Examples**:\n");
            fprintf(fp, "    - User says \"remember his name is Long\" -> Call `memory(history_entry=\"[timestamp] User asked to remember: his name is Long\", memory_update=\"<updated MEMORY.md with new fact>\")`\n");
            fprintf(fp, "    - User says \"my preference is X\" -> Call `memory(history_entry=\"[timestamp] User preference: X\", memory_update=\"<updated>\")`\n");
            fprintf(fp, "    - User shares personal info -> Call `memory(history_entry=\"[timestamp] User info: ...\", memory_update=\"<updated>\")`\n\n");
            fprintf(fp, "**CRITICAL**: Saying \"I've remembered that\" or \"Noted\" in text WITHOUT calling the `memory` tool means the information will be LOST after this session. You MUST call the tool.\n");
            fclose(fp);
            printf("[dim]Created AGENTS.md[/dim]\n");
        }
    }

    /* SOUL.md */
    char soul_path[512];
    snprintf(soul_path, sizeof(soul_path), "%s/SOUL.md", workspace_path);
    if (!file_exists(soul_path)) {
        FILE* fp = fopen(soul_path, "w");
        if (fp) {
            fprintf(fp, "# Soul\n\n");
            fprintf(fp, "You are a lightweight AI assistant.\n\n");
            fprintf(fp, "## Personality\n\n");
            fprintf(fp, "- Helpful and friendly\n");
            fprintf(fp, "- Concise and to the point\n");
            fprintf(fp, "- Curious and eager to learn\n\n");
            fprintf(fp, "## Values\n\n");
            fprintf(fp, "- Accuracy over speed\n");
            fprintf(fp, "- User privacy and safety\n");
            fprintf(fp, "- Transparency in actions\n\n");
            fprintf(fp, "## Communication\n\n");
            fprintf(fp, "- Reply directly with text for conversations\n");
            fprintf(fp, "- Only use specific channel tools (telegram, dingtalk, etc.) to send to a chat channel\n");
            fclose(fp);
            printf("[dim]Created SOUL.md[/dim]\n");
        }
    }

    /* USER.md */
    char user_path[512];
    snprintf(user_path, sizeof(user_path), "%s/USER.md", workspace_path);
    if (!file_exists(user_path)) {
        FILE* fp = fopen(user_path, "w");
        if (fp) {
            fprintf(fp, "# User\n\n");
            fprintf(fp, "## About\n\n");
            fprintf(fp, "- Name: (user's name)\n");
            fprintf(fp, "- Language: (preferred language)\n");
            fprintf(fp, "- Timezone: (your timezone)\n\n");
            fprintf(fp, "## Preferences\n\n");
            fprintf(fp, "- Communication style: (casual/formal)\n");
            fclose(fp);
            printf("[dim]Created USER.md[/dim]\n");
        }
    }

    /* Create memory directory and MEMORY.md */
    char memory_dir[512];
    snprintf(memory_dir, sizeof(memory_dir), "%s/memory", workspace_path);
    create_dir(memory_dir);

    char memory_path[512];
    snprintf(memory_path, sizeof(memory_path), "%s/MEMORY.md", memory_dir);
    if (!file_exists(memory_path)) {
        FILE* fp = fopen(memory_path, "w");
        if (fp) {
            fprintf(fp, "# Long-term Memory\n\n");
            fprintf(fp, "This file stores important information that should persist across sessions.\n\n");
            fprintf(fp, "## User Information\n\n");
            fprintf(fp, "(Important facts about the user)\n\n");
            fprintf(fp, "## Preferences\n\n");
            fprintf(fp, "(User preferences learned over time)\n\n");
            fprintf(fp, "## Important Notes\n\n");
            fprintf(fp, "(Things to remember)\n");
            fclose(fp);
            printf("[dim]Created memory/MEMORY.md[/dim]\n");
        }
    }

    printf("\nPrimagen is ready!\n");
    printf("Next steps:\n");
    printf("  1. Add your API key to %s\n", config_path);
    printf("  2. Run: ./build/primagen agent\n");

    return 0;
}

/* Start the gateway server - placeholder implementation */
int cmd_gateway(Config* cfg, int port, bool verbose) {
    (void)cfg;
    printf("Starting primagen gateway on port %d...\n", port);
    if (verbose) {
        printf("Verbose mode enabled.\n");
    }
    printf("[yellow]Gateway mode is currently a placeholder in this C implementation.[/yellow]\n");
    printf("In full version, this would start an HTTP/WebSocket server.\n");
    return 2;
}

/* Run agent interaction - simplified version for CLI */
int cmd_agent(Config* cfg, const char* message, const char* session_id, bool markdown, bool logs) {
    (void)cfg;
    (void)markdown;
    (void)logs;

    printf("Starting agent interaction...\n");
    printf("Session ID: %s\n", session_id);

    /* This function runs a simplified version of main() but just for one turn or interactive mode */

    if (message) {
        printf("Message: %s\n", message);
        printf("[yellow]Single-shot message mode not fully refactored yet. Use interactive mode.[/yellow]\n");
        return 2;
    } else {
        printf("Entering interactive mode...\n");
        /* This is basically what 'main' does by default now */
    }

    return 0;
}

/* Show channel status */
int cmd_channels_status(Config* cfg) {
    printf("Channel Status\n");
    printf("--------------\n");

    // Channels are now managed as plugins
    // Display plugin status for channel plugins
    if (cfg->plugins.count == 0) {
        printf("No channel plugins configured.\n");
        return 0;
    }

    for (size_t i = 0; i < cfg->plugins.count; i++) {
        PluginConfig* pc = &cfg->plugins.items[i];
        if (pc->plugin_id) {
            printf("%s: %s\n", pc->plugin_id, pc->enabled ? "Enabled" : "Disabled");
        }
    }

    return 0;
}

/* Show primagen status */
int cmd_status(Config* cfg, const char* config_path, const char* workspace_path) {
    printf("Primagen Status\n");
    printf("---------------\n");

    printf("Config:    %s (%s)\n", config_path, file_exists(config_path) ? "Exists" : "Missing");
    printf("Workspace: %s (%s)\n", workspace_path, file_exists(workspace_path) ? "Exists" : "Missing");

    ProviderConfig* active_pc = config_get_active_provider(cfg);
    printf("Provider:  %s\n", cfg->agent.provider);
    printf("Model:     %s\n", active_pc ? active_pc->model : "(none)");
    printf("Heartbeat: %s (%ds)\n", cfg->heartbeat.enabled ? "Enabled" : "Disabled", cfg->heartbeat.interval_s);

    return 0;
}

/* Print command line usage */
void print_usage(const char* program_name) {
    printf("Usage: %s [command] [options]\n\n", program_name);
    printf("Commands:\n");
    printf("  agent     - Run the agent (default)\n");
    printf("  onboard   - Initialize configuration and workspace\n");
    printf("  gateway   - Start the gateway server\n");
    printf("  status    - Show primagen status\n");
    printf("  channels  - Channel management\n\n");
    printf("Options:\n");
    printf("  -c, --config <path>    Path to config file (default: .primagen/config.json)\n");
    printf("  -w, --workspace <path> Path to workspace directory (default: .primagen)\n");
    printf("  -m, --message <msg>    Initial message to send to agent\n");
    printf("  -h, --help             Show this help message\n\n");
    printf("Examples:\n");
    printf("  %s agent -m \"Hello\"\n", program_name);
    printf("  %s onboard\n", program_name);
    printf("  %s gateway\n", program_name);
    printf("  %s status\n", program_name);
}
