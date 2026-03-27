# web_tools

提供 `web_search` 与 `web_fetch` 两个工具。

## config 配置示例

```json
{
  "dns": {
    "dns4": "udp://223.5.5.5:53",
    "dnsTimeoutMs": 10000
  },
  "plugins": [
    {
      "plugin_id": "web_tools",
      "enabled": true,
      "config": {
        "search_enabled": true,
        "search_api_key": "your_search_api_key"
      }
    }
  ]
}
```

## 字段说明

- `plugin_id`: 固定为 `web_tools`
- `enabled`: 必须为 `true` 才会加载
- `config.search_enabled`: 是否开启 `web_search`
- `config.search_api_key`: 搜索 API Key
- `dns.*`: 全局 DNS，可选；不配置时使用 Mongoose 默认 DNS
