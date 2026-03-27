# telegram_channel

用于接入 Telegram 收发消息。

## config 配置示例

```json
{
  "dns": {
    "dns4": "udp://223.5.5.5:53",
    "dnsTimeoutMs": 10000
  },
  "plugins": [
    {
      "plugin_id": "telegram_channel",
      "enabled": true,
      "config": {
        "token": "123456:telegram_bot_token"
      }
    }
  ]
}
```

## 字段说明

- `plugin_id`: 固定为 `telegram_channel`
- `enabled`: 必须为 `true` 才会加载
- `config.token`: Telegram Bot Token
- `dns.*`: 全局 DNS，可选；不配置时使用 Mongoose 默认 DNS
