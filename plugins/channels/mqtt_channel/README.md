# MQTT Channel Plugin

A plugin that provides MQTT channel support for Primagen. This plugin allows Primagen to subscribe to MQTT topics and process incoming messages, as well as publish messages to MQTT topics.

## Features

- Connect to MQTT brokers (MQTT v3.1.1 and v5.0 supported via Mongoose)
- Subscribe to multiple topics
- Receive messages from subscribed topics and process them through Primagen
- Publish messages to MQTT topics
- Support for username/password authentication
- QoS levels 0, 1, and 2

## Prerequisites

- An MQTT broker (e.g., Mosquitto, EMQX, HiveMQ)
- Network access to the MQTT broker

## Building

```bash
cd plugins/channels/mqtt_channel
make
```

Or build all channel plugins:

```bash
cd plugins
make channels
```

## Installation

```bash
cd plugins/channels/mqtt_channel
make install
```

This will copy the `mqtt_channel.so` file to `.primagen/plugins/channels/`.

## Configuration

Add the MQTT channel configuration to your `.primagen/config.json`:

```json
{
  "plugins": [
    {
      "plugin_id": "mqtt_channel",
      "enabled": true,
      "config": {
        "broker_url": "mqtt://localhost:1883",
        "client_id": "primagen_client",
        "username": "your_username",
        "password": "your_password",
        "topics": [
          "primagen/commands",
          "home/sensors/#"
        ]
      }
    }
  ]
}
```

### Configuration Options

- `broker_url` (required): The MQTT broker URL. Examples:
  - `mqtt://localhost:1883` - Standard MQTT
  - `mqtts://localhost:8883` - MQTT over TLS
  - `ws://localhost:8083/mqtt` - MQTT over WebSocket
  - `wss://localhost:8084/mqtt` - MQTT over secure WebSocket

- `client_id` (optional): Client identifier for the MQTT connection. If not provided, a default ID will be used.

- `username` (optional): Username for broker authentication.

- `password` (optional): Password for broker authentication.

- `topics` (required): Array of topics to subscribe to. Can be a single string or an array of strings. Supports MQTT wildcards:
  - `+` - Single-level wildcard
  - `#` - Multi-level wildcard

## Usage

### Receiving Messages

Messages received on subscribed topics will be forwarded to Primagen's agent loop for processing. The topic serves as the chat_id for routing.

Example: If you subscribe to `home/sensors/#` and receive a message on `home/sensors/temperature`:

- Channel: `mqtt`
- Chat ID: `home/sensors/temperature`
- Content: The message payload

### Sending Messages

To send a message via MQTT, configure your agent to send an outbound message with channel `mqtt` and the topic as the chat_id.

```json
{
  "channel": "mqtt",
  "chat_id": "home/actuators/lights",
  "content": "Turn on the lights"
}
```

## MQTT Topic Examples

Here are some common MQTT topic patterns:

- `home/sensors/#` - Subscribe to all sensor data
- `home/sensors/temperature` - Subscribe to temperature readings
- `primagen/commands` - Commands for Primagen
- `device/+/status` - Status updates from all devices
- `iot/+/telemetry` - Telemetry data from all IoT devices

## Troubleshooting

### Connection Issues

1. Verify the broker URL is correct and the broker is running
2. Check network connectivity to the broker
3. For TLS/SSL connections, ensure certificates are properly configured
4. Check broker logs for authentication/authorization issues

### Message Not Received

1. Verify the topic subscription is active
2. Check that the publisher is sending to the correct topic
3. Ensure QoS levels are compatible
4. Look at Primagen logs for any errors

### Message Not Sent

1. Verify the MQTT connection is established
2. Check that the topic exists and is writable
3. Review broker access control policies

## Dependencies

This plugin uses the Mongoose networking library (already included in Primagen) which provides:
- MQTT 3.1.1 and 5.0 client support
- TLS/SSL support
- WebSocket transport support

## See Also

- [Mongoose MQTT Documentation](https://mongoose.ws/documentation/mqtt/)
- [MQTT Protocol Documentation](https://mqtt.org/)
- [Primagen Plugin System](https://github.com/your-repo/primagen)
