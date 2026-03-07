#include "include/common.h"
#include "include/logger.h"
#include "include/message.h"
#include "bus/message_bus.h"
#include "session/session.h"
#include "context/context_builder.h"
#include "tools/tool.h"
#include "tools/tools_impl.h"
#include "agent/agent_loop.h"
#include "providers/llm_provider.h"
#include "include/config.h"
#include "include/subagent.h"
#include "include/cron.h"
#include "include/skills.h"
#include "include/channel.h"
#include "include/commands.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <curl/curl.h>
#include <sys/stat.h>

// Global bus reference for cron callback
static MessageBus* global_bus = NULL;

// Channel list
#define MAX_CHANNELS 10
static Channel* channels[MAX_CHANNELS];
static int channel_count = 0;

void cron_callback(CronJob* job) {
    if (!global_bus) return;
    
    log_info("[Cron] Triggering job: %s", job->name);
    
    // Inject message into bus (Outbound for direct delivery)
    OutboundMessage* msg = outbound_message_new(
        job->channel ? job->channel : "cli",
        job->to ? job->to : "local_user",
        job->payload_message ? job->payload_message : "Cron trigger"
    );
    message_bus_send_outbound(global_bus, msg);
}

void* agent_thread(void* arg) {
    AgentLoop* loop = (AgentLoop*)arg;
    agent_loop_run(loop);
    return NULL;
}

void* outbound_thread(void* arg) {
    MessageBus* bus = (MessageBus*)arg;
    while (1) {
        OutboundMessage* outbound = message_bus_receive_outbound(bus);
        if (outbound) {
            // Dispatch to channels
            for (int i = 0; i < channel_count; i++) {
                if (channels[i]->send) {
                    channels[i]->send(channels[i], outbound);
                }
            }
            outbound_message_free(outbound);
        }
    }
    return NULL;
}

void print_usage(const char* prog_name) {
    printf("Usage: %s [command] [options]\n", prog_name);
    printf("\nCommands:\n");
    printf("  onboard          Initialize configuration and workspace\n");
    printf("  agent            Run the agent loop (default)\n");
    printf("  gateway          Start the gateway server\n");
    printf("  status           Show system status\n");
    printf("  channels status  Show channel status\n");
    printf("\nOptions:\n");
    printf("  -c, --config <path>      Path to config file (default: .primagen/config.json)\n");
    printf("  -w, --workspace <path>   Path to workspace directory (default: .primagen)\n");
    printf("  -m, --message <text>     Initial message to send to the agent\n");
    printf("  -h, --help               Show this help message\n");
}

// Function prototypes for running the agent loop (extracted from original main)
int run_agent_loop(Config* cfg, const char* workspace_path, const char* initial_message);

int main(int argc, char* argv[]) {
    // Default paths
    char* config_path = strdup(".primagen/config.json");
    char* workspace_path = strdup(".primagen");
    char* initial_message = NULL;
    
    // Command parsing
    // We need to handle commands before getopt because getopt might get confused
    // Simple approach: look at argv[1]
    
    char* command = NULL;
    if (argc > 1 && argv[1][0] != '-') {
        command = argv[1];
    }

    // Parse options (skipping command if present)
    int opt_start = command ? 2 : 1;
    // Reset getopt
    optind = opt_start;
    
    static struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {"workspace", required_argument, 0, 'w'},
        {"message", required_argument, 0, 'm'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    int long_index = 0;
    while ((opt = getopt_long(argc, argv, "c:w:m:h", long_options, &long_index)) != -1) {
        switch (opt) {
            case 'c':
                free(config_path);
                config_path = strdup(optarg);
                break;
            case 'w':
                free(workspace_path);
                workspace_path = strdup(optarg);
                break;
            case 'm':
                free(initial_message);
                initial_message = strdup(optarg);
                break;
            case 'h':
                print_usage(argv[0]);
                free(config_path);
                free(workspace_path);
                free(initial_message);
                return 0;
            default:
                // Ignore unknown options for now, or handle error
                break;
        }
    }

    // Load Config (needed for most commands)
    Config* cfg = config_create();
    // Only load if not 'onboard' command (which creates it)
    bool config_loaded = false;
    if (!command || strcmp(command, "onboard") != 0) {
        config_loaded = config_load_from_file(cfg, config_path);
        if (!config_loaded) {
            // Only warn if we expected to load it
            // printf("Warning: Could not load config from %s\n", config_path);
        }
    }

    int ret = 0;

    if (!command || strcmp(command, "agent") == 0) {
        if (!config_loaded) printf("Warning: Could not load config, using defaults.\n");
        ret = run_agent_loop(cfg, workspace_path, initial_message);
    } else if (strcmp(command, "onboard") == 0) {
        ret = cmd_onboard(config_path, workspace_path);
    } else if (strcmp(command, "gateway") == 0) {
        // TODO: parse port/verbose
        ret = cmd_gateway(cfg, 18790, false);
    } else if (strcmp(command, "status") == 0) {
        ret = cmd_status(cfg, config_path, workspace_path);
    } else if (strcmp(command, "channels") == 0) {
        if (argc > 2 && strcmp(argv[2], "status") == 0) {
            ret = cmd_channels_status(cfg);
        } else {
            printf("Unknown channels command. Try: channels status\n");
            ret = 1;
        }
    } else {
        printf("Unknown command: %s\n", command);
        print_usage(argv[0]);
        ret = 1;
    }

    config_destroy(cfg);
    free(config_path);
    free(workspace_path);
    free(initial_message);
    return ret;
}

// Extracted logic for running the agent loop
int run_agent_loop(Config* cfg, const char* workspace_path, const char* initial_message) {
    log_info("Primagen - AI Agent Framework (C Refactoring)");
    log_info("=============================================");

    // Initialize Global Libraries
    curl_global_init(CURL_GLOBAL_ALL);

    char full_log_path[512];
    snprintf(full_log_path, sizeof(full_log_path), "%s/log", workspace_path);
    mkdir(full_log_path, 0755);
    snprintf(full_log_path, sizeof(full_log_path), "%s/log/primagen.log", workspace_path);
    logger_init(full_log_path);

    // 2. Initialize Components
    MessageBus* bus = message_bus_new();
    global_bus = bus;
    
    SessionManager* session_mgr = session_manager_new(workspace_path); 
    ContextBuilder* ctx_builder = context_builder_new(workspace_path); 
    
    // Initialize Memory
    Memory* memory = memory_new();
    context_builder_set_memory(ctx_builder, memory);
    
    // Load Bootstrap Files (Identity & Docs)
    char bootstrap_path[512];
    const char* bootstrap_files[] = {"IDENTITY.md", "AGENTS.md", "SOUL.md", "USER.md", "TOOLS.md"};
    size_t bootstrap_count = sizeof(bootstrap_files) / sizeof(bootstrap_files[0]);

    for (size_t i = 0; i < bootstrap_count; i++) {
        snprintf(bootstrap_path, sizeof(bootstrap_path), "%s/%s", workspace_path, bootstrap_files[i]);
        FILE* fp = fopen(bootstrap_path, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long len = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (len > 0) {
                char* content = malloc(len + 1);
                if (content) {
                    fread(content, 1, len, fp);
                    content[len] = '\0';
                    
                    if (strcmp(bootstrap_files[i], "IDENTITY.md") == 0) {
                        context_builder_set_identity(ctx_builder, content);
                    } else {
                        context_builder_add_bootstrap(ctx_builder, content);
                    }
                    free(content);
                    log_info("[System] Loaded bootstrap file: %s", bootstrap_files[i]);
                }
            }
            fclose(fp);
        }
    }

    ToolRegistry* tool_reg = tool_registry_new();

    // Initialize Channels
    // Console Channel (always enabled for now, or could be configured)
    Channel* console = channel_create_console();
    if (console->init(console, cfg, bus)) {
        channels[channel_count++] = console;
    } else {
        console->destroy(console);
    }
    
    // Telegram Channel
    if (cfg->channels.telegram.enabled) {
        Channel* telegram = channel_create_telegram();
        if (telegram->init(telegram, cfg, bus)) {
            channels[channel_count++] = telegram;
            log_info("[System] Telegram channel initialized.");
        } else {
            telegram->destroy(telegram);
            log_error("[System] Telegram channel disabled or failed to init.");
        }
    }

    // Email Channel
    if (cfg->channels.email.enabled) {
        Channel* email = channel_create_email();
        if (email->init(email, cfg, bus)) {
            channels[channel_count++] = email;
            log_info("[System] Email channel initialized.");
        } else {
            email->destroy(email);
        }
    }

    // Discord Channel
    if (cfg->channels.discord.enabled) {
        Channel* discord = channel_create_discord();
        if (discord->init(discord, cfg, bus)) {
            channels[channel_count++] = discord;
            log_info("[System] Discord channel initialized.");
        } else {
            discord->destroy(discord);
        }
    }

    // Slack Channel
    if (cfg->channels.slack.enabled) {
        Channel* slack = channel_create_slack();
        if (slack->init(slack, cfg, bus)) {
            channels[channel_count++] = slack;
            log_info("[System] Slack channel initialized.");
        } else {
            slack->destroy(slack);
        }
    }

    // DingTalk Channel
    if (cfg->channels.dingtalk.enabled) {
        Channel* dingtalk = channel_create_dingtalk();
        if (dingtalk->init(dingtalk, cfg, bus)) {
            channels[channel_count++] = dingtalk;
            log_info("[System] DingTalk channel initialized.");
        } else {
            dingtalk->destroy(dingtalk);
        }
    }

    // Feishu Channel
    if (cfg->channels.feishu.enabled) {
        Channel* feishu = channel_create_feishu();
        if (feishu->init(feishu, cfg, bus)) {
            channels[channel_count++] = feishu;
            log_info("[System] Feishu channel initialized.");
        } else {
            feishu->destroy(feishu);
        }
    }

    // Start Channels
    log_info("[System] Active Channels (%d):", channel_count);
    for (int i = 0; i < channel_count; i++) {
        if (channels[i]->start) channels[i]->start(channels[i]);
        log_info("  - %s", channels[i]->name);
    }

    // Initialize Subagent Manager
    SubagentManager* subagent_mgr = subagent_manager_create(
        (void*)llm_provider_call,
        workspace_path, 
        bus,
        cfg
    );
    
    // Initialize Cron Service
    char cron_path[512];
    snprintf(cron_path, sizeof(cron_path), "%s/cron_store.json", workspace_path);
    CronService* cron_service = cron_service_create(cron_path);
    cron_service_set_callback(cron_service, cron_callback);
    cron_service_start(cron_service);

    // Initialize Skills Loader
    SkillsLoader* skills_loader = skills_loader_create(workspace_path);

    // Create Tool Context
    ToolContext* tool_ctx = malloc(sizeof(ToolContext));
    tool_ctx->bus = bus;
    tool_ctx->subagent_mgr = subagent_mgr;
    tool_ctx->cron_service = cron_service;
    tool_ctx->skills_loader = skills_loader;
    tool_ctx->memory = memory;
    tool_ctx->workspace = workspace_path;

    // 3. Register Tools
    register_all_tools(tool_reg, tool_ctx);

    // 4. Create Agent Loop
    AgentLoop* loop = agent_loop_new(session_mgr, ctx_builder, tool_reg, bus, cfg);
    agent_loop_set_llm_provider(loop, llm_provider_call);

    // 5. Start Threads
    pthread_t agent_tid, outbound_tid;
    
    if (pthread_create(&agent_tid, NULL, agent_thread, loop) != 0) {
        fprintf(stderr, "Failed to create agent thread\n");
        return 1;
    }
    
    if (pthread_create(&outbound_tid, NULL, outbound_thread, bus) != 0) {
        fprintf(stderr, "Failed to create outbound thread\n");
        return 1;
    }

    // Inject initial message if provided
    if (initial_message) {
        log_info("[System] Injecting initial message: %s", initial_message);
        // Use "cli" channel and "user" chat_id
        InboundMessage* msg = inbound_message_new("cli", "local_user", initial_message);
        message_bus_send_inbound(bus, msg);
    }

    // Main thread waits (Channels run in their own threads or main loop)
    // Console channel spawns a thread, so we just wait here
    pthread_join(agent_tid, NULL);
    // pthread_join(outbound_tid, NULL); // Unreachable unless agent stops

    // Cleanup
    for (int i = 0; i < channel_count; i++) {
        channels[i]->stop(channels[i]);
        channels[i]->destroy(channels[i]);
    }
    
    cron_service_stop(cron_service);
    cron_service_destroy(cron_service);
    subagent_manager_destroy(subagent_mgr);
    skills_loader_destroy(skills_loader);
    memory_free(memory);
    free(tool_ctx);

    curl_global_cleanup();
    logger_cleanup();
    
    return 0;
}
