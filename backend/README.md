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