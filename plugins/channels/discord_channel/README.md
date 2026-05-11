# Discord Channel

Discord bot channel. Receives messages from Discord servers and sends agent replies.

## Configuration

```jsonc
{
  "channels": {
    "discord": {
      "enabled": false,
      "bot_token": "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    }
  }
}
```

| Field | Description |
|-------|-------------|
| `bot_token` | Discord Bot Token from Developer Portal |

## Setup

1. Create a bot at [Discord Developer Portal](https://discord.com/developers/applications)
2. Enable Message Content Intent under Bot settings
3. Generate invite URL with `bot` + `Send Messages` + `Read Message History` scopes
4. Invite bot to your server
5. Copy bot token to config

## Troubleshooting

| Problem | Solution |
|---------|----------|
| Bot not reading messages | Enable "Message Content Intent" in Developer Portal |
| Bot not joining server | Verify invite URL has correct permissions |
| Rate limiting | Discord has rate limits; the bot handles them automatically |