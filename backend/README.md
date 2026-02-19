# MQTT Telemetry Backend

Cloud backend for IoT flood mitigation system - MQTT broker and telemetry subscriber.

## Quick Start
```bash
cd backend
cp .env.example .env
# Edit .env with your credentials
docker compose up -d
```

See main project [README.md](../README.md) for overall architecture.

## Detailed Setup

### Local Development
```bash
# 1. Configure environment
cp .env.example .env
nano .env

# 2. Create password file -> run in powershell
docker run -it --rm -v "${PWD}\mosquitto\config:/mosquitto/config" eclipse-mosquitto:2.0 sh -c "mosquitto_passwd -c -b /mosquitto/config/passwd subscriber_client test1"

# 3. Start services
docker compose up -d

# 4. View logs
docker compose logs -f subscriber
```

## Common Commands
```bash
# Start
docker compose up -d

# Stop
docker compose down

# Logs
docker compose logs -f
docker compose logs -f subscriber

# Restart
docker compose restart subscriber

# Rebuild
docker compose up -d --build

# Status
docker compose ps
```

# Production Deployment Information

## VM Details
- **Public IP:** 150.230.122.17
- **Region:** UK South (London)
- **Shape:** VM.Standard.E2.1.Micro
- **OS:** Ubuntu 22.04 LTS

## Access Information

### SSH Access
Prior to running this command please message @j-wilsons so that I can add your ssh keys to the vm
```bash
ssh -i /path/to/oracle-mqtt-vm.pem ubuntu@150.230.122.17
```

### MQTT Broker
- **Host:** 150.230.122.17
- **Port:** 1883 (None TLS for the momment)
- **Authentication:** Required

## Credentials

**SENSITIVE**

- **Subscriber Username:** subscriber_client
- **Subscriber Password:** [contact @j-wilsons]
- **Publisher Username:** publisher_client
- **Publisher Password:** [contact @j-wilsons - needed for ESP32]

## Quick Commands
```bash
# SSH to VM
ssh -i key.pem ubuntu@150.230.122.17

# Navigate to project
cd ~/iot_project/backend

# View logs
docker-compose logs -f subscriber

# Restart services
docker-compose restart

# Update from git
git pull origin main
docker-compose up -d --build

# Test publish
mosquitto_pub -h localhost -p 1883 -u publisher_client -P 'PASSWORD' \
  -t controller/telemetry \
  -m '{"deviceId":"test","ts":"2026-02-16T20:00:00Z","temperature":22.0,"status":"ok"}'
```

## Deployed: [D18/02/2026]