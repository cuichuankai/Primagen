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
#include "plugin/plugin_manager.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include "vendor/mongoose/mongoose.h"
#include <sys/stat.h>

/* Global bus reference for cron callback */
static MessageBus* global_bus = NULL;

/* Channel list */
#define MAX_CHANNELS 10
static Channel* channels[MAX_CHANNELS];
static int channel_count = 0;

/* Cron job callback - injects message into bus for delivery */
void cron_callback(CronJob* job) {
    if (!global_bus) return;

    log_info("[Cron] Triggering job: %s", job->name);

    InboundMessage* msg = inbound_message_new(
        job->channel ? job->channel : "cli",
        job->to ? job->to : "local_user",
        job->payload_message ? job->payload_message : "Cron trigger"
    );
    message_bus_send_inbound(global_bus, msg);
}

/* Agent thread entry point */
void* agent_thread(void* arg) {
    AgentLoop* loop = (AgentLoop*)arg;
    agent_loop_run(loop);
    return NULL;
}

/* Outbound message dispatcher thread - sends messages to channels */
void* outbound_thread(void* arg) {
    MessageBus* bus = (MessageBus*)arg;
    log_debug("[OutboundThread] Started, channel_count=%d", channel_count);
    while (1) {
        OutboundMessage* outbound = message_bus_receive_outbound(bus);
        if (!outbound) {
            if (message_bus_is_outbound_closed(bus)) {
                break;
            }
            continue;
        }
        log_debug("[OutboundThread] Sending message to channel=%s, chat_id=%s",
                 outbound->channel.data, outbound->chat_id.data);
        int sent = 0;
        bool broadcast = strcmp(outbound->channel.data, "*") == 0 || strcmp(outbound->channel.data, "all") == 0;
        for (int i = 0; i < channel_count; i++) {
            if (!channels[i]->send) continue;
            bool direct_match = strcmp(channels[i]->name, outbound->channel.data) == 0;
            bool cli_console_alias = strcmp(outbound->channel.data, "cli") == 0 && strcmp(channels[i]->name, "console") == 0;
            if (broadcast || direct_match || cli_console_alias) {
                channels[i]->send(channels[i], outbound);
                sent++;
                log_debug("[OutboundThread] Sent to channel %d: %s", i, channels[i]->name);
            }
        }
        if (sent == 0) {
            log_error("[OutboundThread] No channels available to send message!");
        }
        outbound_message_free(outbound);
    }
    log_debug("[OutboundThread] Stopped");
    return NULL;
}

/* Print command line usage */
int run_agent_loop(Config* cfg, const char* workspace_path, const char* initial_message);

/* Main entry point - parses command line arguments and dispatches to appropriate handler */
int main(int argc, char* argv[]) {
    /* Default paths */
    char* config_path = strdup(".primagen/config.json");
    char* workspace_path = strdup(".primagen");
    char* initial_message = NULL;

    /* Command parsing - handle commands before getopt because getopt might get confused */
    char* command = NULL;
    if (argc > 1 && argv[1][0] != '-') {
        command = argv[1];
    }

    /* Parse options (skipping command if present) */
    int opt_start = command ? 2 : 1;
    /* Reset getopt */
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
                /* Ignore unknown options for now, or handle error */
                break;
        }
    }

    /* Load Config (needed for most commands) */
    Config* cfg = config_create();
    if (!cfg) {
        fprintf(stderr, "Failed to create config\n");
        free(config_path);
        free(workspace_path);
        free(initial_message);
        return 1;
    }
    /* Only load if not 'onboard' command (which creates it) */
    bool config_loaded = false;
    if (!command || strcmp(command, "onboard") != 0) {
        config_loaded = config_load_from_file(cfg, config_path);
        if (!config_loaded) {
            /* Only warn if we expected to load it */
        }
    }

    printf("\e[1;34m\n");
    printf("_____________________________  _________________________________   __\n");
    printf("___  __ \\__  __ \\___  _/__   |/  /__    |___  ____/__  ____/__  | / /\n");
    printf("__  /_/ /_  /_/ /__  / __  /|_/ /__  /| |__  / __ __  __/  __   |/ / \n");
    printf("_  ____/_  _, _/__/ /  _  /  / / _  ___ | / /_/ / _  /___  _  /|  /  \n");
    printf("/_/     /_/ |_| /___/  /_/  /_/  /_/  |_| \\____/  /_____/  /_/ |_/   \n");
    printf("\e[0m\n");

    int ret = 0;

    if (!command || strcmp(command, "agent") == 0) {
        if (!config_loaded) printf("Warning: Could not load config, using defaults.\n");
        ret = run_agent_loop(cfg, workspace_path, initial_message);
    } else if (strcmp(command, "onboard") == 0) {
        ret = cmd_onboard(config_path, workspace_path);
    } else if (strcmp(command, "gateway") == 0) {
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

/* Extracted logic for running the agent loop */
int run_agent_loop(Config* cfg, const char* workspace_path, const char* initial_message) {
    int rc = 0;
    ToolContext* tool_ctx = NULL;
    AgentLoop* loop = NULL;
    pthread_t agent_tid = 0, outbound_tid = 0;
    bool agent_thread_started = false;
    bool outbound_thread_started = false;
    printf("      Primagen(Primitive Genesis) - AI Agent Framework\n");
    printf("===================================================================\n");
    mg_log_set(MG_LL_INFO); /* Set mongoose log level if needed */

    char full_log_path[512];
    snprintf(full_log_path, sizeof(full_log_path), "%s/log", workspace_path);
    mkdir(full_log_path, 0755);
    snprintf(full_log_path, sizeof(full_log_path), "%s/log/primagen.log", workspace_path);
    logger_init(full_log_path);
    /* Apply log config */
    logger_set_config(cfg->log.level, cfg->log.console_output);
    log_info("========================================================");
    log_info("[System] Primagen initializing...");

    /* Initialize Components */
    log_debug("[System] Creating MessageBus...");
    MessageBus* bus = message_bus_new();
    global_bus = bus;

    log_debug("[System] Creating SessionManager...");
    SessionManager* session_mgr = session_manager_new(workspace_path);

    log_debug("[System] Creating ContextBuilder...");
    ContextBuilder* ctx_builder = context_builder_new(workspace_path);

    /* Initialize Memory */
    log_debug("[System] Creating Memory...");
    Memory* memory = memory_new_with_config(cfg);
    context_builder_set_memory(ctx_builder, memory);

    /* Load Bootstrap Files (Identity & Docs) */
    log_debug("[System] Loading bootstrap files...");
    char bootstrap_path[512];
    const char* bootstrap_files[] = {"AGENTS.md", "SOUL.md", "USER.md", "TOOLS.md"};
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
                    
                    if (strcmp(bootstrap_files[i], "AGENTS.md") == 0) {
                        context_builder_set_identity(ctx_builder, content);
                    } else {
                        context_builder_add_bootstrap(ctx_builder, content);
                    }
                    free(content);
                    log_debug("[System] Loaded bootstrap file: %s", bootstrap_files[i]);
                }
            }
            fclose(fp);
        }
    }

    ToolRegistry* tool_reg = tool_registry_new();

    /* Initialize Plugin Manager */
    log_debug("[System] Creating PluginManager...");
    PluginManager* plugin_mgr = plugin_manager_new(workspace_path);
    if (plugin_mgr) {
        plugin_mgr->config = cfg;
        plugin_mgr->bus = bus;
        plugin_mgr->tool_registry = tool_reg;
        plugin_mgr->agent_loop = NULL;
        plugin_mgr->session_mgr = NULL;
        plugin_mgr->channel_array = channels;
        plugin_mgr->channel_count_ptr = &channel_count;
        plugin_mgr->channel_capacity = MAX_CHANNELS;
    } else {
        log_error("[System] Failed to create PluginManager");
    }

    /* Initialize Subagent Manager */
    log_debug("[System] Creating SubagentManager...");
    SubagentManager* subagent_mgr = subagent_manager_create(
        (void*)llm_provider_call,
        workspace_path,
        bus,
        cfg
    );

    /* Initialize Cron Service */
    log_debug("[System] Creating CronService...");
    char cron_path[512];
    snprintf(cron_path, sizeof(cron_path), "%s/cron_store.json", workspace_path);
    CronService* cron_service = cron_service_create(cron_path);
    cron_service_set_callback(cron_service, cron_callback);
    cron_service_start(cron_service);

    /* Initialize Skills Loader */
    log_debug("[System] Creating SkillsLoader...");
    SkillsLoader* skills_loader = skills_loader_create(workspace_path);

    /* Create Tool Context */
    log_debug("[System] Creating ToolContext...");
    tool_ctx = malloc(sizeof(ToolContext));
    if (!tool_ctx) {
        log_error("[System] Failed to allocate ToolContext");
        rc = 1;
        goto cleanup;
    }
    tool_ctx->magic = 0x50474E31;
    tool_ctx->bus = bus;
    tool_ctx->subagent_mgr = subagent_mgr;
    tool_ctx->cron_service = cron_service;
    tool_ctx->skills_loader = skills_loader;
    tool_ctx->memory = memory;
    tool_ctx->config = cfg;
    tool_ctx->workspace = workspace_path;
    tool_ctx->current_channel = "cli";
    tool_ctx->current_chat_id = "current";

    /* Register built-in tools with PluginManager */
    log_debug("[System] Registering builtin tools...");
    agent_loop_register_builtin_tools(plugin_mgr, tool_ctx);
    log_debug("[System] Registered builtin tools");

    /* Register built-in channels with PluginManager */
    log_debug("[System] Registering builtin channels...");
    agent_loop_register_builtin_channels(plugin_mgr, cfg);
    log_debug("[System] Registered builtin channels");

    /* Create Agent Loop */
    log_debug("[System] Creating AgentLoop...");
    loop = agent_loop_new(session_mgr, ctx_builder, tool_reg, bus, cfg, plugin_mgr, workspace_path);
    if (!loop) {
        log_error("[System] Failed to create AgentLoop");
        rc = 1;
        goto cleanup;
    }
    agent_loop_set_llm_provider(loop, llm_provider_call);

    if (plugin_mgr) {
        plugin_mgr->agent_loop = loop;
        plugin_mgr->session_mgr = session_mgr;
        log_debug("[Plugin] Loading external plugins...");
        plugin_manager_load_external(plugin_mgr);
    }

    /* Register built-in commands with PluginManager */
    agent_loop_register_builtin_commands(loop);

    /* Start Channels */
    log_debug("[System] Active Channels (%d):", channel_count);
    if (channel_count == 0) {
        log_error("[System] No channels registered! Check channel initialization.");
    }
    for (int i = 0; i < channel_count; i++) {
        if (channels[i]->start) channels[i]->start(channels[i]);
        log_debug("  - %s", channels[i]->name);
    }

    /* Start Threads */
    log_debug("[System] Starting agent thread...");

    if (pthread_create(&agent_tid, NULL, agent_thread, loop) != 0) {
        fprintf(stderr, "Failed to create agent thread\n");
        rc = 1;
        goto cleanup;
    }
    agent_thread_started = true;

    log_debug("[System] Starting outbound thread...");
    if (pthread_create(&outbound_tid, NULL, outbound_thread, bus) != 0) {
        fprintf(stderr, "Failed to create outbound thread\n");
        rc = 1;
        goto cleanup;
    }
    outbound_thread_started = true;

    /* Inject initial message if provided */
    if (initial_message) {
        log_debug("[System] Injecting initial message: %s", initial_message);
        /* Use "cli" channel and "local_user" chat_id */
        InboundMessage* msg = inbound_message_new("cli", "local_user", initial_message);
        message_bus_send_inbound(bus, msg);
    }

    /* Main thread waits (Channels run in their own threads or main loop) */
    /* Console channel spawns a thread, so we just wait here */
    pthread_join(agent_tid, NULL);
    agent_thread_started = false;
    message_bus_close(bus);
    pthread_join(outbound_tid, NULL);
    outbound_thread_started = false;

cleanup:
    if (rc != 0 && loop && agent_thread_started) {
        agent_loop_stop(loop);
    }
    if (agent_thread_started) {
        pthread_join(agent_tid, NULL);
    }
    if (outbound_thread_started) {
        message_bus_close(bus);
        pthread_join(outbound_tid, NULL);
    }

    /* Cleanup */
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

    // Cleanup plugin manager
    if (plugin_mgr) {
        plugin_manager_free(plugin_mgr);
    }
    if (loop && !agent_thread_started) {
        agent_loop_free(loop);
    }

    /* curl_global_cleanup(); - Removed for Mongoose migration */
    logger_cleanup();

    return rc;
}
