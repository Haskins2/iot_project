# Water System — Full Setup Guide

Complete guide to running the IoT water level monitoring and actuation system.
Covers the MQTT backend, web dashboard, and ESP32 device setup.

## Architecture

```
ESP32 (mTLS:8883)──┐
                    ├──► Mosquitto Broker ──► Actuation Service
Test scripts ───────┘       │    │                  │
                            │    │       ┌──────────┘
                            │    ▼       ▼
              Dashboard (ws:8884)   actuate commands
```

- **Port 8883** — MQTT with mutual TLS (devices and backend services)
- **Port 8884** — Plain WebSocket (anonymous read-only, for the dashboard)

## Prerequisites

- Docker and Docker Compose
- Node.js 18+ and npm (for the dashboard dev server)
- SSH access to the VM (contact @j-wilsons to add your key)

## 1. Backend Setup

### 1a. Generate TLS Certificates

Run once from the `backend/` directory on the VM:

```bash
cd ~/iot_project/backend

# Generate the CA (only needed once)
bash scripts/generate-certs.sh broker 150.230.122.17

# Generate service certs
bash scripts/generate-certs.sh service subscriber-service
bash scripts/generate-certs.sh service actuation-service

# Generate device cert (CN must match the device_id in its MQTT topics)
bash scripts/generate-certs.sh device esp32-master-01
```

Fix certificate permissions for Mosquitto:

```bash
sudo chown 1883:1883 mosquitto/certs/broker.key mosquitto/certs/ca.key
sudo chmod 600 mosquitto/certs/broker.key mosquitto/certs/ca.key
```

### 1b. Start the Backend

```bash
cd ~/iot_project/backend
docker compose up -d --build
```

Verify all three containers are running:

```bash
docker compose ps
# Expected: mqtt-broker, mqtt-subscriber, mqtt-actuation all "Up"
```

Check the broker started both listeners:

```bash
docker compose logs mosquitto | grep "listen socket"
# Expected:
#   Opening ipv4 listen socket on port 8883.
#   Opening websockets listen socket on port 8884.
```

## 2. Dashboard Setup

### 2a. Install and Run

```bash
cd ~/iot_project/frontend
npm install
npm run dev -- --host 0.0.0.0
```

The dev server starts at `http://localhost:5173`.

### 2b. Accessing the Dashboard

**If you're on the same machine as the broker:** Open `http://localhost:5173` — the broker URL defaults to `ws://localhost:8884`.

**If accessing remotely via SSH tunnel** (required when the VM has no public ingress for 5173/8884):

```bash
ssh -i key.pem -L 5173:localhost:5173 -L 8884:localhost:8884 ubuntu@150.230.122.17
```

Then open `http://localhost:5173` in your browser. The dashboard auto-detects `ws://localhost:8884` as the broker URL.

### 2c. Connect

1. The broker URL field should already show `ws://localhost:8884`
2. Click **Connect** — the badge should turn green ("Connected")
3. Device cards appear automatically as sensor data arrives

## 2d. Actual working commands used in this session

When the VM is remote and you need a local dashboard + broker tunnel, the commands that worked in this setup were:

```bash
# On your Windows machine (PowerShell)
ssh -i key.pem -L 5173:localhost:5173 -L 18884:localhost:8884 ubuntu@150.230.122.17

# Keep this SSH session open while using the dashboard at http://localhost:5173
# In a second shell in the VM or locally, start the frontend (on the VM works too):
cd ~/iot_project/frontend
npm install
npm run dev -- --host 0.0.0.0

# In the dashboard, use broker URL:
ws://localhost:18884
```

If authentication is required (recommended), set these values in `.env`:

```bash
VITE_MQTT_USERNAME=frontend-user
VITE_MQTT_PASSWORD=dashboard
```

Then restart the dashboard server.

## 3. Testing Without an ESP32

You can test the full system by publishing simulated messages via the actuation container.

### Option A: SSH In and Run Directly

SSH into the VM first, then run the command (avoids nested quoting issues):

```bash
ssh -i key.pem ubuntu@150.230.122.17
```

Then on the VM:

```bash
cd ~/iot_project/backend
docker compose exec -T mosquitto mosquitto_pub \
  -h localhost -p 8883 \
  --cafile /mosquitto/certs/ca.crt \
  --cert /mosquitto/certs/actuation-service.crt \
  --key /mosquitto/certs/actuation-service.key \
  -t 'devices/test-sensor-01/water_level' \
  -m '{"water_level":1450,"raindrop_sensor":{"analog":800,"digital":0},"ts":"2026-03-30T12:00:00Z"}' \
  -q 1
```

### Option B: Python Script via the Actuation Container

Create a file called `test_publish.py`:

```python
import paho.mqtt.client as mqtt, ssl, json, time

c = mqtt.Client(client_id='test-pub')
c.tls_set(
    ca_certs='/certs/ca.crt',
    certfile='/certs/actuation-service.crt',
    keyfile='/certs/actuation-service.key'
)
c.tls_insecure_set(True)
c.connect('mosquitto', 8883, 60)
c.loop_start()
time.sleep(2)

def pub(device, level, analog, digital):
    msg = json.dumps({
        'water_level': level,
        'raindrop_sensor': {'analog': analog, 'digital': digital},
        'ts': '2026-03-30T12:00:00Z'
    })
    c.publish(f'devices/{device}/water_level', msg, qos=1)
    print(f'  {device}: water={level} analog={analog} digital={digital}')
    time.sleep(1)

# Normal reading
pub('test-sensor-01', 1450, 800, 0)

# High water + rain → pump activates
pub('test-sensor-01', 1950, 1600, 1)

# Back to normal → pump deactivates
pub('test-sensor-01', 1300, 400, 0)

print('Done')
c.disconnect()
```

Copy to the VM and run:

```bash
scp -i key.pem backend/tests/test_publish.py ubuntu@150.230.122.17:/tmp/
ssh -i key.pem ubuntu@150.230.122.17 "cd ~/iot_project/backend && \
  docker compose exec -T actuation python - < /tmp/test_publish.py"
```

There is also a more comprehensive test script at `backend/tests/test_scenarios.py` that exercises hysteresis, raindrop confirmation, and analog-only rain detection across multiple devices. Run it the same way:

```bash
scp -i key.pem backend/tests/test_scenarios.py ubuntu@150.230.122.17:/tmp/
ssh -i key.pem ubuntu@150.230.122.17 "cd ~/iot_project/backend && \
  docker compose exec -T actuation python - < /tmp/test_scenarios.py"
```

### Expected Behaviour

| Message | Dashboard Shows | Actuation |
|---------|----------------|-----------|
| `water_level: 1450`, no rain | Normal (blue bar ~70%) | Pump OFF |
| `water_level: 1950`, rain detected | HIGH (red bar ~95%), Raindrop: Detected | Pump ACTIVATES |
| `water_level: 1300`, no rain | Normal (blue bar ~63%) | Pump DEACTIVATES |

### Payload Format

```json
{
  "water_level": 1450,
  "raindrop_sensor": {
    "analog": 800,
    "digital": 0
  },
  "ts": "2026-03-30T12:00:00Z"
}
```

- `water_level` — ADC reading (0–4095). Thresholds: activate at 1900, deactivate at 1880 (hysteresis).
- `raindrop_sensor.digital` — 1 = rain detected, 0 = clear.
- `raindrop_sensor.analog` — >= 1500 also counts as rain detected (analog fallback).
- `ts` — ISO 8601 timestamp from the device.

### Actuation Logic

The actuation service evaluates every incoming reading:

1. **Hysteresis** — Pump turns ON when water > 1900, turns OFF only when water < 1880. Readings between 1880–1900 don't change pump state.
2. **Raindrop confirmation** — High water alone won't activate the pump. The raindrop sensor must also confirm rain (digital=1 or analog >= 1500).
3. **Sliding window** — Keeps the last 5 readings per device. Requires a majority of high + rain-confirmed readings to activate.
4. **Spike rejection** — Isolated high readings surrounded by normal values are ignored.

## 4. ESP32 Setup (With Hardware)

> **Note:** The ESP32 firmware currently publishes to `controller/water_level` with payload `{"water_raw": N}`.
> To work with the backend and dashboard, the firmware needs to be updated to publish to `devices/esp32-master-01/water_level` with the payload format above.
> It should also subscribe to `devices/esp32-master-01/actuate` to receive pump commands from the actuation service.

### 4a. Flash Certificates to the ESP32

Copy these files from `backend/mosquitto/certs/` to the ESP32 project's `certs/` directory:

- `ca.crt` — CA certificate (to verify the broker)
- `esp32-master-01.crt` — device certificate
- `esp32-master-01.key` — device private key

### 4b. Configure and Flash

```bash
cd embedded_systems/esp32c6
idf.py set-target esp32c6
idf.py -p PORT flash monitor
```

See `embedded_systems/esp32c6/README.md` for wiring and hardware details.

### 4c. MQTT Topics

The ESP32 should use these topics (the cert CN `esp32-master-01` acts as the MQTT username via `use_identity_as_username`):

| Direction | Topic | Purpose |
|-----------|-------|---------|
| Publish | `devices/esp32-master-01/water_level` | Send sensor readings |
| Subscribe | `devices/esp32-master-01/actuate` | Receive pump on/off commands |

The ACL enforces that each device can only write to `devices/{its-own-CN}/#` and read from `devices/{its-own-CN}/actuate`.

## Troubleshooting

**Dashboard says "Connected" but no device cards appear**
- The ACL may be blocking subscriptions. Ensure the global `topic read devices/#` rule appears at the top of `backend/mosquitto/config/acl` (before any `user` line).
- Publish a test message using the commands above and check if activity appears in the log panel.

**Dashboard can't connect**
- Verify port 8884 is the plain WebSocket listener (no TLS). Check `mosquitto.conf` — the 8884 listener should NOT have `cafile`/`certfile`/`keyfile` lines.
- If accessing remotely, ensure the SSH tunnel forwards port 8884.

**Mosquitto restart loop**
- Usually a permissions issue on `broker.key`. Run: `sudo chown 1883:1883 backend/mosquitto/certs/broker.key`

**ESP32 can't connect**
- Verify the device cert was generated with the correct CN: `openssl x509 -in esp32-master-01.crt -noout -subject`
- Ensure `ca.crt`, device `.crt` and `.key` are all flashed to the ESP32.
- Check broker logs: `docker compose logs mosquitto`

## File Structure

```
backend/
├── docker-compose.yml          # Mosquitto + subscriber + actuation
├── mosquitto/
│   └── config/
│       ├── mosquitto.conf      # Broker config (dual listeners)
│       └── acl                 # Topic access control
├── subscriber/
│   └── subscriber.py           # Logs all device messages
├── actuation/
│   └── actuation.py            # Evaluates readings, publishes pump commands
└── scripts/
    └── generate-certs.sh       # TLS certificate generation

frontend/
├── index.html
├── package.json                # vue 3, mqtt.js 5, vite 5
└── src/
    ├── main.js
    └── App.vue                 # Dashboard (connects via ws://broker:8884)

embedded_systems/
└── esp32c6/
    ├── main/main.c             # Firmware (reads water sensor, publishes MQTT)
    └── certs/                  # Device TLS certificates
```
