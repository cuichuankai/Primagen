---
name: cron
description: Schedule reminders and recurring tasks.
---

# Cron

Use the `cron` tool to schedule reminders or recurring tasks.

## Parameters

`cron(name, payload, schedule, channel?, chat_id?)`

- `name`: job name
- `payload`: reminder content
- `schedule`: `@every N`, `@in N`, `@at TIMESTAMP`, or daily cron `M H * * *`
- `channel`: optional, defaults to current session channel
- `chat_id`: optional, defaults to current session chat_id

## Examples

Fixed reminder:
```
cron(name="coffee-break", payload="Time to take a break!", schedule="@every 1200")
```

One-time reminder:
```
cron(name="meeting-reminder", payload="Remind me about the meeting", schedule="@in 1800")
```

Daily 9:30 reminder:
```
cron(name="morning-coffee", payload="Drink coffee", schedule="30 9 * * *")
```

## Time Expressions

| User says | Parameters |
|-----------|------------|
| every 20 minutes | `schedule: "@every 1200"` |
| every hour | `schedule: "@every 3600"` |
| every day at 8am | `schedule: "0 8 * * *"` |
| 9:30am daily | `schedule: "30 9 * * *"` |
| after 10 minutes | `schedule: "@in 600"` |
| at unix timestamp | `schedule: "@at 1760000000"` |
Daily cron format uses server local timezone.
