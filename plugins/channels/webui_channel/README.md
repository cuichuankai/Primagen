# webui_channel

用于启动内置 Web UI 管理界面。

## config 配置示例

```json
{
  "plugins": [
    {
      "plugin_id": "webui_channel",
      "enabled": true,
      "config": {
        "port": 16714
      }
    }
  ]
}
```

## 字段说明

- `plugin_id`: 固定为 `webui_channel`
- `enabled`: 必须为 `true` 才会加载
- `config.port`: Web UI 服务端口，默认 `16714`
