---
name: cron
description: Schedule reminders and recurring tasks.
always: true
---

# Cron

## Parameters

`cron(name, payload, schedule, channel?, chat_id?)`

- `name`: Unique job identifier (e.g., "drink-water", "stand-up")
- `payload`: Message content to deliver when triggered
- `schedule`:
  - `@in N` - N seconds from now (one-time)
  - `@every N` - Every N seconds (recurring)
  - `M H * * *` - Daily cron (minute hour day month weekday)

## Examples

```
cron(name="drink-water", payload="该喝水了！", schedule="@in 1800")
cron(name="coffee-break", payload="Take a break!", schedule="@every 1200")
cron(name="morning-coffee", payload="Drink coffee", schedule="30 9 * * *")
```

## Time Expressions

| User says | Schedule |
|-----------|----------|
| "10 分钟后提醒我..." | `@in 600` |
| "30 秒后提醒我..." | `@in 30` |
| "每小时提醒我..." | `@every 3600` |
| "每天早上 8 点提醒..." | `0 8 * * *` |

## Important

- **MUST call the cron tool** when user asks to set a reminder. Text-only acknowledgment is NEVER sufficient — the reminder will NOT be set unless you call the tool.
- **Do NOT proactively remind** about tasks you see in conversation history. Only send reminder messages when the cron service triggers them automatically.
- If you see a past reminder in conversation history that has already been delivered, **do not repeat it**.
- When a NEW reminder request comes in, always call the cron tool regardless of any past reminders in history.
