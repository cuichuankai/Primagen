#ifndef BUILTIN_TOOLS_DEF_H
#define BUILTIN_TOOLS_DEF_H

#include "../tools/tools_impl.h"
#include "../tools/tool.h"

typedef struct {
    const char* name;
    const char* desc;
    const char* params;
    ToolExecuteFunc exec;
} BuiltinToolDef;

static const BuiltinToolDef BUILTIN_TOOLS_FULL[] = {
    {"read_file", "Read file content",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        tool_read_file},
    {"write_file", "Write file content",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}",
        tool_write_file},
    {"edit_file", "Edit file content",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old_str\":{\"type\":\"string\"},\"new_str\":{\"type\":\"string\"}},\"required\":[\"path\",\"old_str\",\"new_str\"]}",
        tool_edit_file},
    {"list_dir", "List directory contents",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        tool_list_dir},
    {"exec", "Execute shell command",
        "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}",
        tool_exec},
    {"send_message", "Send message to user. Optional attachments support image/audio/video uploads.",
        "{\"type\":\"object\",\"properties\":{\"content\":{\"type\":\"string\"},\"attachments\":{\"type\":\"array\",\"items\":{\"oneOf\":[{\"type\":\"string\"},{\"type\":\"object\",\"properties\":{\"type\":{\"type\":\"string\",\"enum\":[\"image\",\"audio\",\"video\"]},\"path\":{\"type\":\"string\"},\"duration\":{\"type\":\"integer\",\"minimum\":1},\"cover_path\":{\"type\":\"string\"}},\"required\":[\"type\",\"path\"]}]}}},\"required\":[\"content\"]}",
        tool_send_message},
    {"spawn_subagent", "Spawn subagent",
        "{\"type\":\"object\",\"properties\":{\"task\":{\"type\":\"string\"},\"label\":{\"type\":\"string\"}},\"required\":[\"task\"]}",
        tool_spawn},
    {"cron", "Schedule a reminder or recurring task. When triggered, the payload is sent as a user message to the AI, which then generates and sends the response. The payload should be a clear instruction describing what the AI should do when triggered, NOT the final content itself. Formats: '@in N' (N seconds later, one-time), '@every N' (recurring), '@at TIMESTAMP', or 'M H * * *' (daily cron). Example: name='drink-water', payload='Remind the user to drink water', schedule='@in 1800'. Example: name='daily-news', payload='Search for today\\'s top tech news and format as a daily digest', schedule='0 8 * * *'.",
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Unique job identifier, e.g., 'drink-water', 'daily-news'\"},\"payload\":{\"type\":\"string\",\"description\":\"Instruction for the AI to execute when triggered. Should describe WHAT the AI should do, not the final message content. e.g., 'Search today\\'s tech news and summarize' NOT 'Here is the news: ...'\"},\"schedule\":{\"type\":\"string\",\"description\":\"When to trigger: '@in N' (N seconds), '@every N', '@at UNIX_TIMESTAMP', or 'M H * * *'\"},\"channel\":{\"type\":\"string\"},\"chat_id\":{\"type\":\"string\"}},\"required\":[\"name\",\"payload\",\"schedule\"]}",
        tool_cron},
    {"skill", "Manage skills",
        "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list\",\"load\",\"unload\"]},\"name\":{\"type\":\"string\"}},\"required\":[\"action\"]}",
        tool_skill},
    {"memory", "Manage long-term memory. Use this to consolidate conversation history into persistent memory.",
        "{\"type\":\"object\",\"properties\":{\"history_entry\":{\"type\":\"string\",\"description\":\"A paragraph summarizing key events/decisions. Start with [YYYY-MM-DD HH:MM].\"},\"memory_update\":{\"type\":\"string\",\"description\":\"Full updated long-term memory content (facts). Return unchanged if no new facts.\"}},\"required\":[\"history_entry\"]}",
        tool_memory},
};

#define BUILTIN_TOOLS_FULL_COUNT (sizeof(BUILTIN_TOOLS_FULL) / sizeof(BUILTIN_TOOLS_FULL[0]))

static const BuiltinToolDef BUILTIN_TOOLS_SUBAGENT[] = {
    {"read_file", "Read file content",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        tool_read_file},
    {"write_file", "Write file content",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}",
        tool_write_file},
    {"edit_file", "Edit file content",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old_str\":{\"type\":\"string\"},\"new_str\":{\"type\":\"string\"}},\"required\":[\"path\",\"old_str\",\"new_str\"]}",
        tool_edit_file},
    {"list_dir", "List directory contents",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        tool_list_dir},
    {"exec", "Execute shell command",
        "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}",
        tool_exec},
    {"send_message", "Send message to user. Optional attachments support image/audio/video uploads.",
        "{\"type\":\"object\",\"properties\":{\"content\":{\"type\":\"string\"},\"attachments\":{\"type\":\"array\",\"items\":{\"oneOf\":[{\"type\":\"string\"},{\"type\":\"object\",\"properties\":{\"type\":{\"type\":\"string\",\"enum\":[\"image\",\"audio\",\"video\"]},\"path\":{\"type\":\"string\"},\"duration\":{\"type\":\"integer\",\"minimum\":1},\"cover_path\":{\"type\":\"string\"}},\"required\":[\"type\",\"path\"]}]}}},\"required\":[\"content\"]}",
        tool_send_message},
};

#define BUILTIN_TOOLS_SUBAGENT_COUNT (sizeof(BUILTIN_TOOLS_SUBAGENT) / sizeof(BUILTIN_TOOLS_SUBAGENT[0]))

#endif
