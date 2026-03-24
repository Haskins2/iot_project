# MQTT Telemetry Backend

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
# Start
docker compose up -d

# Stop
docker compose down

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

# Status
docker compose ps
```

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
