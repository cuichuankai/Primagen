# slack_channel

用于把消息发送到 Slack。

## config 配置示例

```json
{
  "dns": {
    "dns4": "udp://223.5.5.5:53",
    "dnsTimeoutMs": 10000
  },
  "plugins": [
    {
      "plugin_id": "slack_channel",
      "enabled": true,
      "config": {
        "bot_token": "xoxb-xxxxxxxx"
      }
    }
  ]
}
```

## 字段说明

- `plugin_id`: 固定为 `slack_channel`
- `enabled`: 必须为 `true` 才会加载
- `config.bot_token`: Slack Bot Token
- `dns.*`: 全局 DNS，可选；不配置时使用 Mongoose 默认 DNS
