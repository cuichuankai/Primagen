# DingTalk Channel Plugin

钉钉机器人频道插件，为 Primagen 提供钉钉消息收发功能。

## 功能特点

- 支持钉钉企业内部机器人
- 自动管理 access_token（2 小时有效期）
- Markdown 格式消息发送
- 支持用户白名单（allowFrom）

## 配置方法

### 1. 创建钉钉机器人

1. 登录 [钉钉开发者后台](https://developers.dingtalk.com/)
2. 创建企业内部应用
3. 获取 `Client ID` 和 `Client Secret`
4. 在应用权限中开通"机器人"相关权限

### 2. 配置文件

编辑 `.primagen/config.json`：

```json
{
  "channels": {
    "dingtalk": {
      "enabled": true,
      "clientId": "your_client_id",
      "clientSecret": "your_client_secret",
      "allowFrom": null
    }
  }
}
```

### 3. 配置说明

| 字段 | 类型 | 说明 |
|------|------|------|
| `enabled` | boolean | 是否启用钉钉频道 |
| `clientId` | string | 钉钉应用的 Client ID |
| `clientSecret` | string | 钉钉应用的 Client Secret |
| `allowFrom` | array | 允许的用户 ID 列表，null 表示允许所有 |

### 4. 环境变量（可选）

也可以使用环境变量覆盖配置文件：

```bash
export PRIMAGEN_DINGTALK_ENABLED=true
export PRIMAGEN_DINGTALK_CLIENT_ID=your_client_id
export PRIMAGEN_DINGTALK_CLIENT_SECRET=your_client_secret
```

## 使用方法

### 发送消息

通过消息总线发送消息到钉钉：

```
channel: dingtalk
chat_id: <用户 UserID>
content: <消息内容>
```

### 接收消息

当前版本仅支持发送消息，接收消息功能需要配合钉钉事件订阅使用。

## 构建和安装

### 构建

```bash
cd plugins/channels/dingtalk_channel
make
```

### 安装

```bash
make install
```

插件会被安装到 `build/.primagen/plugins/`

### 清理

```bash
make clean
```

## API 参考

### 钉钉 API 文档

- [获取 access_token](https://open.dingtalk.com/document/orgapp/obtain-orgapp-token)
- [发送机器人消息](https://open.dingtalk.com/document/orgapp/robot-message)

## 注意事项

1. **Token 管理**: 插件会自动刷新 access_token，有效期为 2 小时
2. **消息格式**: 当前仅支持 Markdown 格式
3. **用户 ID**: 需要使用钉钉企业内的用户 UserID
4. **权限**: 确保应用有足够的权限发送消息

## 故障排除

### 无法获取 Token

- 检查 Client ID 和 Client Secret 是否正确
- 确保应用状态正常
- 检查网络连接

### 消息发送失败

- 检查 access_token 是否有效
- 确认用户 UserID 正确
- 检查应用权限配置

## 版本历史

- v1.0 - 初始版本，支持基本的消息发送功能
