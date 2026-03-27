# hello_command

示例命令插件，注册一个 `hello` 命令。

## config 配置示例

```json
{
  "plugins": [
    {
      "plugin_id": "hello_command",
      "enabled": true,
      "config": {}
    }
  ]
}
```

## 字段说明

- `plugin_id`: 固定为 `hello_command`
- `enabled`: 必须为 `true` 才会加载
- `config`: 目前无可配置字段，需保留空对象
