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
#include <signal.h>

#include <stdatomic.h>

/* Global loop reference for signal handler - using atomic for thread safety */
static _Atomic(AgentLoop*) g_loop = NULL;
static volatile sig_atomic_t g_shutdown_requested = 0;

/* Signal handler: must be async-signal-safe. We only:
 *   - set a sig_atomic_t flag (always safe)
 *   - call agent_loop_request_stop which only does atomic_store
 * We do NOT call message_bus_close or pthread_cond_broadcast here. */
static void handle_signal(int sig) {
    (void)sig;
    g_shutdown_requested = 1;
    AgentLoop* loop = atomic_load(&g_loop);
    if (loop) {
        agent_loop_request_stop(loop);
    }
}

/* Cron job callback - injects message into bus for delivery */
void cron_callback(CronJob* job, void* user_data) {
    MessageBus* bus = (MessageBus*)user_data;
    if (!bus) return;

    log_info("[Cron] Triggering job: %s", job->name);

    InboundMessage* msg = inbound_message_new(
        job->channel ? job->channel : "cli",
        job->to ? job->to : "local_user",
        job->payload_message ? job->payload_message : "Cron trigger",
        NULL
    );
    msg->no_session_record = true;
    message_bus_send_inbound(bus, msg);
}

/* Agent thread entry point */
void* agent_thread(void* arg) {
    AgentLoop* loop = (AgentLoop*)arg;
    agent_loop_run(loop);
    return NULL;
}

typedef struct {
    MessageBus* bus;
    DynamicArray* channels;
} OutboundThreadArgs;

/* Outbound message dispatcher thread - sends messages to channels */
void* outbound_thread(void* arg) {
    OutboundThreadArgs* args = (OutboundThreadArgs*)arg;
    MessageBus* bus = args->bus;
    DynamicArray* channels = args->channels;
    log_debug("[OutboundThread] Started");
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
        for (size_t i = 0; i < channels->count; i++) {
            Channel* ch = *(Channel**)dynamic_array_get(channels, i);
            if (!ch->send) continue;
            bool direct_match = strcmp(ch->name, outbound->channel.data) == 0;
            bool cli_console_alias = strcmp(outbound->channel.data, "cli") == 0 && strcmp(ch->name, "console") == 0;
            if (broadcast || direct_match || cli_console_alias) {
                ch->send(ch, outbound);
                sent++;
                log_debug("[OutboundThread] Sent to channel %zu: %s", i, ch->name);
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
    srand((unsigned int)time(NULL));
    /* Default paths */
    char* config_path = xstrdup(".primagen/config.json");
    char* workspace_path = xstrdup(".primagen");
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
                config_path = xstrdup(optarg);
                break;
            case 'w':
                free(workspace_path);
                workspace_path = xstrdup(optarg);
                break;
            case 'm':
                free(initial_message);
                initial_message = xstrdup(optarg);
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
    DynamicArray* channels = NULL;
    OutboundThreadArgs* outbound_args = NULL;
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
    channels = malloc(sizeof(DynamicArray));
    *channels = dynamic_array_new(sizeof(Channel*));
    log_debug("[System] Creating MessageBus...");
    MessageBus* bus = message_bus_new();

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
                    size_t bytes_read = fread(content, 1, len, fp);
                    content[bytes_read] = '\0';
                    
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

    // Initialize Plugin Manager
    log_debug("[System] Creating PluginManager...");
    PluginManager* plugin_mgr = plugin_manager_new(workspace_path);
    if (plugin_mgr) {
        plugin_mgr->config = cfg;
        plugin_mgr->bus = bus;
        plugin_mgr->tool_registry = tool_reg;
        plugin_mgr->agent_loop = NULL;
        plugin_mgr->session_mgr = NULL;
        plugin_mgr->channels = channels;
    } else {
        log_error("[System] Failed to create PluginManager");
    }

    /* Initialize LLM Provider Registry (auto-registers built-in OpenAI provider) */
    log_debug("[System] Initializing LLM Provider Registry...");
    LLMProviderRegistry* registry = llm_provider_get_registry();

    if (plugin_mgr) {
        log_debug("[Plugin] Loading external plugins...");
        plugin_manager_load_external(plugin_mgr);
    }

    if (registry) {
        if (cfg->agent.provider && strlen(cfg->agent.provider) > 0) {
            llm_provider_registry_set_active(registry, cfg->agent.provider);
        }
    }
    LLMProvider* active_provider = registry ? llm_provider_registry_get_active(registry) : NULL;
    SubagentSharedContext* subagent_shared = subagent_shared_context_create(
        active_provider,
        workspace_path,
        bus,
        cfg
    );
    SubagentManager* subagent_mgr = subagent_manager_create(subagent_shared);

    /* Initialize Cron Service */
    log_debug("[System] Creating CronService...");
    char cron_path[512];
    snprintf(cron_path, sizeof(cron_path), "%s/cron_store.json", workspace_path);
    CronService* cron_service = cron_service_create(cron_path);
    cron_service_set_callback(cron_service, cron_callback, bus);
    cron_service_start(cron_service);

    /* Initialize Skills Loader */
    log_debug("[System] Creating SkillsLoader...");
    SkillsLoader* skills_loader = skills_loader_create(workspace_path);

    /* Create Tool Context */
    log_debug("[System] Creating ToolContext...");
    tool_ctx = tool_context_new(bus, subagent_mgr, cron_service,
                                skills_loader, memory, cfg,
                                plugin_mgr, workspace_path);
    if (!tool_ctx) {
        log_error("[System] Failed to allocate ToolContext");
        rc = 1;
        goto cleanup;
    }

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
    agent_loop_set_llm_provider(loop, active_provider);

    if (plugin_mgr) {
        plugin_mgr->agent_loop = loop;
        plugin_mgr->session_mgr = session_mgr;
    }

    /* Set global loop for signal handler and register signals */
    atomic_store(&g_loop, loop);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /* Register built-in commands with PluginManager - already done in agent_loop_new */

    /* Start Channels */
    log_debug("[System] Active Channels (%zu):", channels->count);
    if (channels->count == 0) {
        log_error("[System] No channels registered! Check channel initialization.");
    }
    for (size_t i = 0; i < channels->count; i++) {
        Channel* ch = *(Channel**)dynamic_array_get(channels, i);
        if (ch->start) ch->start(ch);
        log_debug("  - %s", ch->name);
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
    outbound_args = calloc(1, sizeof(OutboundThreadArgs));
    if (!outbound_args) {
        fprintf(stderr, "Failed to allocate outbound thread args\n");
        rc = 1;
        goto cleanup;
    }
    outbound_args->bus = bus;
    outbound_args->channels = channels;
    
    if (pthread_create(&outbound_tid, NULL, outbound_thread, outbound_args) != 0) {
        fprintf(stderr, "Failed to create outbound thread\n");
        free(outbound_args);
        rc = 1;
        goto cleanup;
    }
    outbound_thread_started = true;

    /* Inject initial message if provided */
    if (initial_message) {
        log_debug("[System] Injecting initial message: %s", initial_message);
        /* Use "cli" channel and "local_user" chat_id */
        InboundMessage* msg = inbound_message_new("cli", "local_user", initial_message, NULL);
        if (msg) message_bus_send_inbound(bus, msg);
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
    if (channels) {
        for (size_t i = 0; i < channels->count; i++) {
            Channel* ch = *(Channel**)dynamic_array_get(channels, i);
            if (ch->stop) ch->stop(ch);
            if (ch->destroy) ch->destroy(ch);
        }
        dynamic_array_free(channels);
        free(channels);
    }
    
    free(outbound_args);

    cron_service_stop(cron_service);
    cron_service_destroy(cron_service);
    subagent_manager_destroy(subagent_mgr);
    subagent_shared_context_destroy(subagent_shared);
    skills_loader_destroy(skills_loader);
    memory_free(memory);
    free(tool_ctx);

    // Cleanup plugin manager
    if (plugin_mgr) {
        plugin_manager_free(plugin_mgr);
    }
    if (loop) {
        agent_loop_free(loop);
    }

    /* curl_global_cleanup(); - Removed for Mongoose migration */
    logger_cleanup();

    return rc;
}
