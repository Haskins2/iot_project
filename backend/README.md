# MQTT Telemetry Backend

Cloud backend for the IoT flood mitigation system — Mosquitto MQTT broker, telemetry subscriber, and water level actuation service.

## Architecture

Three Docker containers:

| Container | Purpose |
|-----------|---------|
| `mqtt-broker` | Mosquitto 2.0 — mTLS on port 8883, plain WebSocket on port 8884 |
| `mqtt-subscriber` | Logs all messages from `devices/+/water_level` |
| `mqtt-actuation` | Evaluates water levels and publishes pump commands to `devices/{id}/actuate` |

## Quick Start

### 1. Generate TLS Certificates

```bash
cd backend

# CA + broker cert
bash scripts/generate-certs.sh broker 150.230.122.17

# Backend service certs
bash scripts/generate-certs.sh service subscriber-service
bash scripts/generate-certs.sh service actuation-service

# Device cert (one per ESP32 — CN must match device_id)
bash scripts/generate-certs.sh device esp32-master-01

# Fix key permissions for Mosquitto
sudo chown 1883:1883 mosquitto/certs/broker.key mosquitto/certs/ca.key
sudo chmod 600 mosquitto/certs/broker.key mosquitto/certs/ca.key
```

### 2. Start Services

```bash
docker compose up -d --build
```

### 3. Verify

```bash
docker compose ps          # All three should be "Up"
docker compose logs -f     # Watch all logs
```

## Common Commands

```bash
docker compose up -d                  # Start
docker compose down                   # Stop
docker compose logs -f                # All logs
docker compose logs -f actuation      # Actuation logs only
docker compose restart mosquitto      # Restart broker
docker compose up -d --build          # Rebuild after code changes
```

## Broker Configuration

- **Port 8883** — MQTT with mutual TLS. Clients must present a certificate signed by the project CA. The cert CN becomes the MQTT username via `use_identity_as_username`.
- **Port 8884** — Plain WebSocket. Anonymous read-only access for the web dashboard.
- **ACL** — `mosquitto/config/acl` controls topic access per user/device.

## Actuation Logic

The actuation service (`actuation/actuation.py`) evaluates every `water_level` message:

1. **Hysteresis** — ON threshold: 1900, OFF threshold: 1880. Prevents pump flip-flop.
2. **Raindrop confirmation** — High water alone won't activate. Requires `digital=1` or `analog >= 1500`.
3. **Sliding window** — Last 5 readings per device. Majority must be high + rain-confirmed.
4. **Spike rejection** — Isolated anomalous readings are ignored.

## Test Publishing

```bash
# From inside the VM
docker compose exec -T mosquitto mosquitto_pub \
  -h localhost -p 8883 \
  --cafile /mosquitto/certs/ca.crt \
  --cert /mosquitto/certs/actuation-service.crt \
  --key /mosquitto/certs/actuation-service.key \
  -t 'devices/test-sensor-01/water_level' \
  -m '{"water_level":1950,"raindrop_sensor":{"analog":1600,"digital":1},"ts":"2026-03-30T12:00:00Z"}' \
  -q 1
```

## VM Details

- **Public IP:** 150.230.122.17
- **Region:** UK South (London)
- **Shape:** VM.Standard.E2.1.Micro
- **OS:** Ubuntu 22.04 LTS

### SSH Access

Contact @j-wilsons to add your SSH key, then:

```bash
ssh -i key.pem ubuntu@150.230.122.17
```

## Update Deployed Code

```bash
ssh -i key.pem ubuntu@150.230.122.17
cd ~/iot_project
git pull origin UI
cd backend
docker compose up -d --build
```