# email_channel

用于通过 IMAP 收信、SMTP 发信。

## config 配置示例

```json
{
  "plugins": [
    {
      "plugin_id": "email_channel",
      "enabled": true,
      "config": {
        "imap_host": "imap.example.com",
        "imap_user": "bot@example.com",
        "imap_pass": "your_imap_password",
        "smtp_host": "smtp.example.com",
        "smtp_user": "bot@example.com",
        "smtp_pass": "your_smtp_password"
      }
    }
  ]
}
```

## 字段说明

- `plugin_id`: 固定为 `email_channel`
- `enabled`: 必须为 `true` 才会加载
- `config.imap_host`: IMAP 服务器地址
- `config.imap_user`: IMAP 用户名
- `config.imap_pass`: IMAP 密码
- `config.smtp_host`: SMTP 服务器地址
- `config.smtp_user`: SMTP 用户名
- `config.smtp_pass`: SMTP 密码
