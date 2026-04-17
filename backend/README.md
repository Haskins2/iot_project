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
Cloud backend for IoT flood mitigation system — MQTT broker, subscriber, and actuation service.


## Services

| Container | Role |
|---|---|
| `mqtt-broker` | Mosquitto 2.0 broker, mTLS on port 8883 |
| `mqtt-subscriber` | Subscribes to `devices/+/water_level`, logs received data |
| `mqtt-actuation` | Subscribes to `devices/+/water_level`, publishes commands to `devices/{id}/actuate` |

## Local Development

### Prerequisites
- Docker + Docker Compose
- OpenSSL (via WSL or Linux)

### Setup

```bash
cd backend

# 1. Generate certs (run from backend/ — not from inside scripts/)
bash scripts/generate-certs.sh broker localhost
bash scripts/generate-certs.sh service subscriber-service
bash scripts/generate-certs.sh service actuation-service

# 2. Start services
docker compose up -d --build
```

### Testing the full feedback loop

Generate a test device cert, then publish a water level message:

```bash
bash scripts/generate-certs.sh device esp-test

docker exec mqtt-broker mosquitto_pub \
  --cafile /mosquitto/certs/ca.crt \
  --cert   /mosquitto/certs/esp-test.crt \
  --key    /mosquitto/certs/esp-test.key \
  -h localhost -p 8883 \
  -t devices/esp-test/water_level \
  -m '{"deviceId":"esp-test","water_level":42,"status":"ok","ts":"2026-03-24T00:00:00Z"}'
```

Check logs to verify both services received the message and actuation published a command:

```bash
docker compose logs -f subscriber
docker compose logs -f actuation
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
# Logs (all services)
docker compose logs -f

# Logs (single service)
docker compose logs -f subscriber
docker compose logs -f actuation

# Restart single service
docker compose restart mosquitto
docker compose restart actuation

# Rebuild after code changes
docker compose up -d --build

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

## Certificate Management

Certs are generated once per environment and never committed to git.

```bash
# CA + broker cert (use VM public IP on the VM, localhost locally)
bash scripts/generate-certs.sh broker <hostname-or-ip>

# Backend service certs
bash scripts/generate-certs.sh service subscriber-service
bash scripts/generate-certs.sh service actuation-service

# Per-device cert (repeat for each physical device)
bash scripts/generate-certs.sh device <device-id>
```

`ca.crt` is the only file distributed to devices. All `*.key` files are private — never commit them.

## Production Deployment (Oracle VM)

### VM Details
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
- **Port:** 8883 (mTLS)

### SSH Access
Contact @j-wilsons to have your SSH key added to the VM.
```bash
ssh -i /path/to/oracle-mqtt-vm.pem ubuntu@150.230.122.17
```

### First-time VM Setup

```bash
cd ~/iot_project/backend

# 1. Generate certs using the VM's public IP
bash scripts/generate-certs.sh broker 150.230.122.17
bash scripts/generate-certs.sh service subscriber-service
bash scripts/generate-certs.sh service actuation-service

# 2. Fix file ownership so the mosquitto container (uid 1883) can read them
sudo chown -R 1883:1883 mosquitto/certs
sudo chown 1883:1883 mosquitto/config/acl
sudo chmod 700 mosquitto/config/acl

# 3. Start services
docker compose up -d --build
```

### Updating the VM

```bash
cd ~/iot_project/backend
git pull origin main
docker compose up -d --build
```

### Adding a new device

```bash
bash scripts/generate-certs.sh device <device-id>
# Copy <device-id>.crt, <device-id>.key, and ca.crt onto the device's filesystem
```
