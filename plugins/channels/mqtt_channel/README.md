# MQTT Channel

MQTT publish/subscribe channel. Receives messages from MQTT topics and publishes agent replies.

## Configuration

```jsonc
{
  "channels": {
    "mqtt": {
      "enabled": false,
      "broker": "tcp://localhost:1883",
      "client_id": "primagen-agent",
      "username": "",
      "password": "",
      "subscribe_topic": "primagen/in",
      "publish_topic": "primagen/out"
    }
  }
}
```

| Field | Description |
|-------|-------------|
| `broker` | MQTT broker URL (tcp://, ssl://, ws://, wss://) |
| `client_id` | MQTT client identifier |
| `username` | Broker authentication username (optional) |
| `password` | Broker authentication password (optional) |
| `subscribe_topic` | Topic to listen for incoming messages |
| `publish_topic` | Topic to publish outgoing replies |

## Usage

```bash
# Send a message to the agent
mosquitto_pub -h localhost -t "primagen/in" -m '{"content":"Hello"}'

# Listen for replies
mosquitto_sub -h localhost -t "primagen/out"
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| Connection refused | Verify broker URL and port; check MQTT broker is running |
| Auth fails | Check username/password; some brokers require explicit anonymous access |
| Messages not received | Verify subscribe_topic matches the publishing topic |