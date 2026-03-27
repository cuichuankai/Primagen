# stdout_channel

用于将消息直接输出到标准输出。

## config 配置示例

```json
{
  "plugins": [
    {
      "plugin_id": "stdout_channel",
      "enabled": true,
      "config": {}
    }
  ]
}
```

## 字段说明

- `plugin_id`: 固定为 `stdout_channel`
- `enabled`: 必须为 `true` 才会加载
- `config`: 目前无可配置字段，需保留空对象
