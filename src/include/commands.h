#ifndef COMMANDS_H
#define COMMANDS_H

#include "common.h"
#include "config.h"

// Command functions
int cmd_onboard(const char* config_path, const char* workspace_path);
int cmd_gateway(Config* cfg, int port, bool verbose);
int cmd_agent(Config* cfg, const char* message, const char* session_id, bool markdown, bool logs);
int cmd_channels_status(Config* cfg);
int cmd_status(Config* cfg, const char* config_path, const char* workspace_path);

#endif // COMMANDS_H
