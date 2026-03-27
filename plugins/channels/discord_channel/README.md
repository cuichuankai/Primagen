# discord_channel

用于把消息发送到 Discord 频道。

## config 配置示例

```json
{
  "dns": {
    "dns4": "udp://223.5.5.5:53",
    "dnsTimeoutMs": 10000
  },
  "plugins": [
    {
      "plugin_id": "discord_channel",
      "enabled": true,
      "config": {
        "token": "discord_bot_token",
        "channel_id": "1234567890"
      }
    }
  ]
}
```

## 字段说明

- `plugin_id`: 固定为 `discord_channel`
- `enabled`: 必须为 `true` 才会加载
- `config.token`: Discord Bot Token
- `config.channel_id`: 默认频道 ID
- `dns.*`: 全局 DNS，可选；不配置时使用 Mongoose 默认 DNS
