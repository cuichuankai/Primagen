# echo_tool

示例工具插件，提供 `echo` 工具。

## config 配置示例

```json
{
  "plugins": [
    {
      "plugin_id": "echo_tool",
      "enabled": true,
      "config": {}
    }
  ]
}
```

## 字段说明

- `plugin_id`: 固定为 `echo_tool`
- `enabled`: 必须为 `true` 才会加载
- `config`: 目前无可配置字段，需保留空对象
