#ifndef AGENT_COMMON_H
#define AGENT_COMMON_H

#include <stddef.h>
#include <string.h>
#include <ctype.h>

static inline void parse_slash_command(const char* full_content, char* cmd_name, size_t cmd_name_size, const char** args_start_out) {
    if (!full_content || !cmd_name || !args_start_out) return;

    while (*full_content && isspace((unsigned char)*full_content)) full_content++;
    if (*full_content != '/') return;

    const char* cmd_start = full_content + 1;

    const char* cmd_end = strchr(cmd_start, ' ');
    if (cmd_end) {
        size_t cmd_len = cmd_end - cmd_start;
        if (cmd_len < cmd_name_size) {
            strncpy(cmd_name, cmd_start, cmd_len);
            cmd_name[cmd_len] = '\0';
        } else {
            strncpy(cmd_name, cmd_start, cmd_name_size - 1);
            cmd_name[cmd_name_size - 1] = '\0';
        }
        *args_start_out = cmd_end + 1;
    } else {
        strncpy(cmd_name, cmd_start, cmd_name_size - 1);
        cmd_name[cmd_name_size - 1] = '\0';
        *args_start_out = NULL;
    }
}

#endif
