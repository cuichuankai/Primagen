#include "../include/commands.h"
#include "../include/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Helper to check if file exists
static bool file_exists(const char* path) {
    return access(path, F_OK) != -1;
}

// Helper to create directory
static void create_dir(const char* path) {
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        mkdir(path, 0755);
    }
}

int cmd_onboard(const char* config_path, const char* workspace_path) {
    printf("Initializing primagen configuration and workspace...\n");

    // 1. Create Config Directory
    char config_dir[512];
    char* last_slash = strrchr(config_path, '/');
    if (last_slash) {
        size_t len = last_slash - config_path;
        strncpy(config_dir, config_path, len);
        config_dir[len] = '\0';
        create_dir(config_dir);
    }

    // 2. Create/Check Config File
    if (file_exists(config_path)) {
        printf("[yellow]Config already exists at %s[/yellow]\n", config_path);
        // Simple logic: just warn, don't overwrite unless force (not implemented)
    } else {
        Config* cfg = config_create();
        if (config_save_to_file(cfg, config_path)) {
            printf("[green]✓[/green] Created config at %s\n", config_path);
        } else {
            printf("[red]Failed to create config at %s[/red]\n", config_path);
        }
        config_destroy(cfg);
    }

    // 3. Create Workspace
    create_dir(workspace_path);
    printf("[green]✓[/green] Workspace ready at %s\n", workspace_path);

    // 4. Create Templates (Simplified)
    char path[512];
    snprintf(path, sizeof(path), "%s/IDENTITY.md", workspace_path);
    if (!file_exists(path)) {
        FILE* fp = fopen(path, "w");
        if (fp) {
            fprintf(fp, "# Identity\nYou are Primagen, an AI assistant.\n");
            fprintf(fp, "# Core Memory Instructions\n");
            fprintf(fp, "You represent a long-term companion. To maintain continuity across sessions, you MUST proactively manage your memory.\n");

            fprintf(fp, "1.  **Save Facts**: When the user provides important personal information (name, preferences, project details), IMMEDIATELY use the `memory` tool to save it.\n");
            fprintf(fp, "    - Example: User says \"Call me Chuck\", you call `memory(content=\"User's name is Chuck\")`.\n");
            fprintf(fp, "2.  **Consolidate History**: If a conversation covers important decisions or events, use the `memory` tool with `history_entry` to save a summary.\n");
            fprintf(fp, "    - Example: User says \"I'm working on a project\", you call `memory(content=\"User is working on a project\")`.\n");
            fprintf(fp, "3.  **Check Memory**: Always refer to the \"Long-term Memory\" section in your context to personalize your responses.\n"); 
            fclose(fp);
            printf("[green]✓[/green] Created IDENTITY.md\n");
        }
    }

    printf("\nPrimagen is ready!\n");
    printf("Next steps:\n");
    printf("  1. Add your API key to %s\n", config_path);
    printf("  2. Run: ./build/primagen agent\n");

    return 0;
}

int cmd_gateway(Config* cfg, int port, bool verbose) {
    (void)cfg;
    printf("Starting primagen gateway on port %d...\n", port);
    if (verbose) {
        printf("Verbose mode enabled.\n");
    }
    printf("[yellow]Gateway mode is currently a placeholder in this C implementation.[/yellow]\n");
    printf("In full version, this would start an HTTP/WebSocket server.\n");
    // TODO: Implement actual server loop
    return 0;
}

int cmd_agent(Config* cfg, const char* message, const char* session_id, bool markdown, bool logs) {
    (void)cfg;
    (void)markdown;
    (void)logs;

    printf("Starting agent interaction...\n");
    printf("Session ID: %s\n", session_id);
    
    // This function essentially runs a simplified version of main()
    // but just for one turn or interactive mode
    
    if (message) {
        printf("Message: %s\n", message);
        // TODO: Inject message into bus and run loop for one turn
        printf("[yellow]Single-shot message mode not fully refactored yet. Use interactive mode.[/yellow]\n");
    } else {
        printf("Entering interactive mode...\n");
        // This is basically what 'main' does by default now
    }
    
    return 0;
}

int cmd_channels_status(Config* cfg) {
    printf("Channel Status\n");
    printf("--------------\n");
    
    printf("Telegram: %s\n", cfg->channels.telegram.enabled ? "Enabled" : "Disabled");
    if (cfg->channels.telegram.enabled) {
        printf("  Token: %s\n", strlen(cfg->channels.telegram.token) > 5 ? "******" : "Not Set");
    }
    
    printf("WhatsApp: %s\n", cfg->channels.whatsapp.enabled ? "Enabled" : "Disabled");
    if (cfg->channels.whatsapp.enabled) {
        printf("  Bridge URL: %s\n", cfg->channels.whatsapp.bridge_url);
    }
    
    return 0;
}

int cmd_status(Config* cfg, const char* config_path, const char* workspace_path) {
    printf("Primagen Status\n");
    printf("---------------\n");
    
    printf("Config:    %s (%s)\n", config_path, file_exists(config_path) ? "Exists" : "Missing");
    printf("Workspace: %s (%s)\n", workspace_path, file_exists(workspace_path) ? "Exists" : "Missing");
    
    printf("Model:     %s\n", cfg->agent.model);
    printf("Heartbeat: %s (%ds)\n", cfg->heartbeat.enabled ? "Enabled" : "Disabled", cfg->heartbeat.interval_s);
    
    return 0;
}
