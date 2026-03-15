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

- **Do NOT proactively remind** about tasks you see in conversation history.
- Scheduled tasks are handled automatically by the cron service.
- If you see a past reminder in history (e.g., "30 秒后提醒我喝水"), **do not repeat it**.
- Only send reminders when:
  - The cron service triggers them (automatic)
  - The user explicitly asks you to remind them NOW
