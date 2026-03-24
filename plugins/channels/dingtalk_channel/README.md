# DingTalk Channel Plugin

This plugin adds DingTalk message send/receive support to Primagen.

## Capabilities

- Sends outbound messages to DingTalk
- Supports image/audio/video attachment upload and delivery
- Receives inbound messages through DingTalk WebSocket streaming
- Automatically refreshes and reuses `access_token`
- Supports session-webhook reply mode for better threaded responses

## Configuration

Configure the plugin in `.primagen/config.json` under `plugins`:

```json
{
  "plugins": [
    {
      "plugin_id": "dingtalk_channel",
      "enabled": true,
      "clientId": "your_client_id",
      "clientSecret": "your_client_secret"
    }
  ]
}
```

### Fields

| Field          | Type    | Required | Description                        |
| -------------- | ------- | -------- | ---------------------------------- |
| `plugin_id`    | string  | yes      | Must be `dingtalk_channel`         |
| `enabled`      | boolean | yes      | Enables registration and startup   |
| `clientId`     | string  | yes      | DingTalk application client ID     |
| `clientSecret` | string  | yes      | DingTalk application client secret |

## Message Routing

- Outbound channel name must be `dingtalk`.
- `chat_id` can be:
  - `webhook_url|conversation_id` (preferred for replies)
  - `conversation_id` (legacy fallback)

## Attachment Format

`send_message` supports optional `attachments`:

```json
{
  "content": "可选文本说明",
  "attachments": [
    {
      "type": "image",
      "path": "/absolute/path/to/picture.png"
    },
    {
      "type": "audio",
      "path": "/absolute/path/to/audio.mp3",
      "duration": 2000
    },
    {
      "type": "video",
      "path": "/absolute/path/to/video.mp4"
    }
  ]
}
```

## Build and Install

```bash
cd plugins/channels/dingtalk_channel
make
make install
```

Output and install targets:

- Build output: `build/.primagen/plugins/channels/dingtalk_channel.so`
- Runtime install: `.primagen/plugins/channels/dingtalk_channel.so`

## Run

Start Primagen normally after configuration:

```bash
./build/primagen agent
```

The plugin is loaded automatically during startup.

## Quick Start

1. Create an internal DingTalk app bot and obtain `clientId` and `clientSecret`.
2. Enable the "Robot" permissions in the permissions management.
3. Add the `dingtalk_channel` plugin entry in `.primagen/config.json` and set `"enabled": true`.
4. Build and install the plugin:
   ```bash
   cd plugins/channels/dingtalk_channel
   make && make install
   ```
5. Start Primagen:
   ```bash
   ./build/primagen agent
   ```
6. Send one test message through DingTalk and verify both send and receive behavior in the running agent.

## Troubleshooting

- Token refresh fails: verify `clientId` and `clientSecret`.
- Messages do not send: confirm channel is `dingtalk` and plugin is enabled.
- Replies fail in sessions: ensure `chat_id` uses `webhook_url|conversation_id`.
- No inbound events: verify DingTalk app permissions and callback/event setup.

## Reference

- DingTalk Open Platform: <https://developers.dingtalk.com/>
