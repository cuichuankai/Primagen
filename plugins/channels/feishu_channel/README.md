# Feishu Channel Plugin

This plugin adds Feishu (Lark) bot message send/receive support to Primagen.

## Capabilities

- Sends outbound messages to Feishu chats
- Supports image/audio/video attachment upload and delivery
- Receives inbound messages through Feishu WebSocket stream
- Automatically obtains and refreshes tenant access token
- Supports plain text mode and optional card mode (`use_card`)

## Configuration

Configure the plugin in `.primagen/config.json` under `plugins`:

```json
{
  "plugins": [
    {
      "plugin_id": "feishu_channel",
      "enabled": true,
      "app_id": "cli_xxx",
      "app_secret": "xxx",
      "use_card": false
    }
  ]
}
```

### Fields

| Field        | Type    | Required | Description                        |
| ------------ | ------- | -------- | ---------------------------------- |
| `plugin_id`  | string  | yes      | Must be `feishu_channel`           |
| `enabled`    | boolean | yes      | Enables plugin registration        |
| `app_id`     | string  | yes      | Feishu app ID                      |
| `app_secret` | string  | yes      | Feishu app secret                  |
| `use_card`   | boolean | no       | Use card API instead of plain text |

## Message Routing

- Outbound channel name must be `feishu`.
- `chat_id` should be a Feishu receive/chat identifier that the bot can send to.

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
      "path": "/absolute/path/to/audio.opus",
      "duration": 3000
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
cd plugins/channels/feishu_channel
make
make install
```

Output and install targets:

- Build output: `build/.primagen/plugins/channels/feishu_channel.so`
- Runtime install: `.primagen/plugins/channels/feishu_channel.so`

## Run

Start Primagen normally after configuration:

```bash
./build/primagen agent
```

The plugin is loaded automatically during startup.

## Quick Start

1. Create a Feishu app bot in the Feishu Open Platform and get `app_id` and `app_secret`.
2. Enable the "Robot" permissions in the permissions management.
3. Add the `feishu_channel` plugin entry in `.primagen/config.json` and set `"enabled": true`.
4. Build and install the plugin:
   ```bash
   cd plugins/channels/feishu_channel
   make && make install
   ```
5. Start Primagen:
   ```bash
   ./build/primagen agent
   ```
6. Send one test message to your bot, then confirm both outbound and inbound logs/messages are working.

## Troubleshooting

- Plugin does not load: check `plugin_id` and `enabled`.
- Channel does not start: verify `app_id` is configured.
- Send fails: check app credentials and bot permission scope.
- No inbound messages: verify Feishu event subscription and callback setup.

## Reference

- Feishu Open Platform: <https://open.feishu.cn/>
