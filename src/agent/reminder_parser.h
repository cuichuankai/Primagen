#ifndef REMINDER_PARSER_H
#define REMINDER_PARSER_H

#include <stdbool.h>

typedef struct {
    bool valid;
    int delay_seconds;
    char payload[512];
    char name[128];
} ReminderParseResult;

ReminderParseResult reminder_parse_simple(const char* text);
bool reminder_should_fallback_schedule(int turn, int pending_tool_count);

#endif // REMINDER_PARSER_H
