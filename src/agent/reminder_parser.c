#include "reminder_parser.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool text_contains_any(const char* text, const char* const* needles, size_t count) {
    if (!text) return false;
    for (size_t i = 0; i < count; i++) {
        if (needles[i] && strstr(text, needles[i])) return true;
    }
    return false;
}

static void sanitize_job_name(const char* payload, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    size_t n = 0;
    const char* prefix = "reminder";
    while (*prefix && n + 1 < out_size) out[n++] = *prefix++;
    if (payload && payload[0] && n + 1 < out_size) {
        out[n++] = '-';
        for (size_t i = 0; payload[i] && n + 1 < out_size && n < 72; i++) {
            unsigned char c = (unsigned char)payload[i];
            if (isalnum(c)) out[n++] = (char)tolower(c);
            else if (c == ' ' || c == '-' || c == '_') out[n++] = '-';
        }
    }
    while (n > 0 && out[n - 1] == '-') n--;
    out[n] = '\0';
}

static const char* skip_ascii_space(const char* p) {
    while (p && (*p == ' ' || *p == '\t' || *p == ':' || *p == ',' || *p == '.')) p++;
    if (p && strncmp(p, "。", strlen("。")) == 0) p += strlen("。");
    return p;
}

ReminderParseResult reminder_parse_simple(const char* text) {
    ReminderParseResult result;
    memset(&result, 0, sizeof(result));
    if (!text || text[0] == '\0') return result;

    const char* reminder_words[] = {"提醒", "remind", "提醒我"};
    const char* relative_words[] = {"后", "in "};
    if (!text_contains_any(text, reminder_words, sizeof(reminder_words) / sizeof(reminder_words[0])) ||
        !text_contains_any(text, relative_words, sizeof(relative_words) / sizeof(relative_words[0]))) {
        return result;
    }

    const char* p = text;
    int value = 0;
    const char* unit_pos = NULL;
    const char* after_unit = NULL;
    while (*p) {
        if (isdigit((unsigned char)*p)) {
            char* end = NULL;
            long parsed = strtol(p, &end, 10);
            if (parsed <= 0 || parsed > 365L * 24L * 3600L) return result;
            const char* q = skip_ascii_space(end);
            int multiplier = 0;
            if (strncmp(q, "秒", strlen("秒")) == 0 || strncmp(q, "second", 6) == 0 || strncmp(q, "sec", 3) == 0) {
                multiplier = 1;
            } else if (strncmp(q, "分钟", strlen("分钟")) == 0 || strncmp(q, "分", strlen("分")) == 0 || strncmp(q, "minute", 6) == 0 || strncmp(q, "min", 3) == 0) {
                multiplier = 60;
            } else if (strncmp(q, "小时", strlen("小时")) == 0 || strncmp(q, "hour", 4) == 0) {
                multiplier = 3600;
            }
            if (multiplier > 0) {
                value = (int)(parsed * multiplier);
                unit_pos = p;
                after_unit = q;
                const char* hou = strstr(after_unit, "后");
                if (hou && hou - after_unit < 16) {
                    after_unit = hou + strlen("后");
                } else {
                    while (*after_unit && !isspace((unsigned char)*after_unit)) after_unit++;
                }
                break;
            }
            p = end;
            continue;
        }
        p++;
    }
    if (value <= 0 || !unit_pos) return result;

    const char* reminder_pos = strstr(text, "提醒我");
    if (!reminder_pos) reminder_pos = strstr(text, "提醒");
    if (!reminder_pos) reminder_pos = strstr(text, "remind");

    const char* payload_start = NULL;
    if (reminder_pos && reminder_pos > unit_pos) {
        payload_start = reminder_pos;
        if (strncmp(payload_start, "提醒我", strlen("提醒我")) == 0) payload_start += strlen("提醒我");
        else if (strncmp(payload_start, "提醒", strlen("提醒")) == 0) payload_start += strlen("提醒");
        else if (strncmp(payload_start, "remind me", 9) == 0) payload_start += 9;
        else if (strncmp(payload_start, "remind", 6) == 0) payload_start += 6;
    } else if (after_unit) {
        payload_start = after_unit;
    }
    payload_start = skip_ascii_space(payload_start);
    if (!payload_start || payload_start[0] == '\0') {
        payload_start = "该做提醒事项了";
    }

    snprintf(result.payload, sizeof(result.payload), "提醒用户：%s", payload_start);
    size_t len = strlen(result.payload);
    while (len > 0 && (result.payload[len - 1] == '.' ||
                       result.payload[len - 1] == '!' || result.payload[len - 1] == '?' ||
                       result.payload[len - 1] == '\n' || result.payload[len - 1] == '\r')) {
        result.payload[--len] = '\0';
    }
    sanitize_job_name(result.payload, result.name, sizeof(result.name));
    result.delay_seconds = value;
    result.valid = true;
    return result;
}

bool reminder_should_fallback_schedule(int turn, int pending_tool_count) {
    return turn <= 1 && pending_tool_count == 0;
}
