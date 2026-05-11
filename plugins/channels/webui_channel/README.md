# WebUI Channel

A built-in web-based chat console and control panel. Provides a browser interface for interacting with the agent, managing plugins, skills, sessions, cron jobs, and viewing configuration.

## Features

### Chat Console
- Real-time chat with the agent via long-polling
- Markdown rendering with code syntax highlighting
- Image and file attachment uploads
- Session history viewer
- Conversation clearing and session management

### Control Panel
- **Plugins**: View loaded channels/tools/commands, enable/disable, upload `.so` files
- **Skills**: Browse, load/unload, edit, and import skills from local or remote sources
- **Sessions**: List active sessions with message counts and timestamps
- **Usage**: Token usage statistics and estimated cost tracking
- **Cron**: View and manage scheduled cron jobs
- **Config**: Read-only view of the current configuration

### System
- **Logs**: Recent application log entries
- **Documentation**: Inline help and documentation

## Configuration

```jsonc
{
  "channels": {
    "webui": {
      "enabled": true,
      "port": 8090,
      "host": "0.0.0.0"
    }
  }
}
```

## API Endpoints

### Chat

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/api/chat/send` | Send a user message |
| `GET` | `/api/chat/poll` | Long-poll for agent replies (30s timeout) |

**Send message**:
```json
POST /api/chat/send
{
  "chat_id": "session-abc",
  "content": "Hello",
  "images": ["data:image/png;base64,..."]  // optional
}
```

**Poll reply** (long-polling):
```
GET /api/chat/poll?chat_id=session-abc
→ 200 with { "chat_id": "...", "message": "..." }
→ 204 if no reply within 30s (re-poll)
```

### Control Panel

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/control/channels` | List loaded channel plugins |
| `GET` | `/api/control/tools` | List loaded tool plugins |
| `GET` | `/api/control/commands` | List loaded command plugins |
| `POST` | `/api/control/plugin-enable` | Enable/disable a plugin |
| `POST` | `/api/control/plugins/upload` | Upload `.so` plugin file |
| `GET` | `/api/control/skills` | List available skills |
| `GET` | `/api/control/skills/content` | Get skill file content |
| `POST` | `/api/control/skills/save` | Save skill content |
| `POST` | `/api/control/skills/import` | Import skill from URL |
| `GET` | `/api/control/sessions` | List active sessions |
| `GET` | `/api/control/usage` | Token usage statistics |
| `GET` | `/api/control/cron` | List cron jobs |

### System

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/health` | Health check |
| `GET` | `/api/config` | Current configuration |
| `GET` | `/api/settings/logs` | Recent log entries |
| `GET` | `/api/settings/docs` | Documentation |

## User Interface

The WebUI is served as a single-page application. Access it at `http://localhost:8090` (or your configured host:port).

### Navigation
- **Overview** — System status dashboard
- **Chat** — Main chat interface
- **Plugins** — Plugin management (channels/tools/commands tabs)
- **Skills** — Skill browser, editor, and importer
- **Sessions** — Session list with conversation viewer
- **Subagents** — Subagent status monitor
- **Usage** — Token usage charts and cost estimates
- **Cron** — Cron job list and manager
- **Config** — Read-only config viewer
- **Logs** — Recent log viewer
- **Docs** — Built-in documentation

### Theme
- Dark theme by default with CSS variable customization
- Responsive layout for desktop and mobile
- Optimized for modern browsers (Chrome, Firefox, Safari, Edge)

## Troubleshooting

| Problem | Solution |
|---------|----------|
| Page not loading | Verify port is not in use: `lsof -i :8090` |
| Chat not responding | Check agent is running; check `agent.log` for channel errors |
| Plugin upload fails | Verify file is a valid `.so`; check file permissions |
| Skill import fails | Check URL accessibility; verify it points to a SKILL.md |
| CORS errors (external access) | The WebUI serves from the same origin; proxy if needed |