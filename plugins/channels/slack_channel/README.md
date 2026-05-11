# Slack Channel

Slack bot channel. Receives messages from Slack workspaces via Socket Mode and sends agent replies.

## Configuration

```jsonc
{
  "channels": {
    "slack": {
      "enabled": false,
      "bot_token": "xoxb-xxxxxxxxxxxx-xxxxxxxxxxxx-xxxxxxxxxxxxxxxxxxxxxxxx",
      "app_token": "xapp-1-xxxxxxxxxxxx-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    }
  }
}
```

| Field | Description |
|-------|-------------|
| `bot_token` | Slack Bot User OAuth Token (starts with `xoxb-`) |
| `app_token` | Slack App-Level Token (starts with `xapp-`) |

## Setup

1. Create a Slack app at [api.slack.com/apps](https://api.slack.com/apps)
2. Enable Socket Mode
3. Subscribe to `message.channels`, `message.groups`, `message.im`, `message.mpim` events
4. Install app to workspace and copy tokens
5. Enable `slack` channel in config

## Troubleshooting

| Problem | Solution |
|---------|----------|
| Socket connection fails | Verify `app_token` starts with `xapp-`; check Internet access |
| Bot not receiving messages | Verify event subscriptions; check bot is added to channel |
| Thread replies not working | Use `thread_ts` in message context (handled automatically) |