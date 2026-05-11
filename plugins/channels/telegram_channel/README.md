# Telegram Channel

Telegram bot channel. Receives messages via Telegram Bot API and sends agent replies.

## Configuration

```jsonc
{
  "channels": {
    "telegram": {
      "enabled": false,
      "bot_token": "1234567890:ABCdefGHIjklMNOpqrsTUVwxyz"
    }
  }
}
```

| Field | Description |
|-------|-------------|
| `bot_token` | Telegram Bot API token from @BotFather |

## Setup

1. Create a bot via [@BotFather](https://t.me/BotFather) on Telegram
2. Copy the bot token
3. Enable `telegram` channel in config and set `bot_token`

## Features

- Text message replies
- Support for bot commands (`/help`, `/clear`, etc.)
- Long message splitting for messages exceeding Telegram limits

## Troubleshooting

| Problem | Solution |
|---------|----------|
| Bot not responding | Verify bot token is correct; check network connectivity |
| Long messages truncated | Messages are auto-split by the channel if too long |
| Rate limiting | Telegram has rate limits (~30 msg/s); reduce sending frequency |