# Water System Dashboard

Vue 3 web dashboard for real-time monitoring and control of the water level actuation system. Connects to the Mosquitto broker via WebSocket (port 8884).

## Features

- Real-time water level readings with colour-coded bar (blue/yellow/red)
- Raindrop sensor status (digital + analog values)
- Live pump status with activate/deactivate controls
- Stats row: device count, active pumps, high water alerts, message count
- Activity log panel with timestamped events
- Auto-reconnect on connection loss

## Setup

### Prerequisites

- Node.js 18+ and npm
- Mosquitto broker running with WebSocket listener on port 8884

### Install and Run

```bash
cd frontend
npm install
npm run dev -- --host 0.0.0.0
```

Dashboard at `http://localhost:5173`.

### Remote Access via SSH Tunnel

If the VM doesn't have public ingress on ports 5173/8884:

```bash
ssh -i key.pem -L 5173:localhost:5173 -L 8884:localhost:8884 ubuntu@150.230.122.17
```

Then open `http://localhost:5173` — the broker URL auto-detects `ws://localhost:8884`.

## MQTT Topics

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `devices/+/water_level` | Subscribe | Incoming sensor telemetry |
| `devices/+/actuate` | Subscribe | Pump commands from actuation service |
| `devices/{id}/actuate` | Publish | Manual pump control from dashboard |

## Tech Stack

- **Vue 3** — UI framework
- **Vite 5** — Dev server and build
- **mqtt.js 5** — WebSocket MQTT client

## File Structure

```
frontend/
├── index.html          # Entry point (dark gradient background)
├── package.json        # Dependencies
└── src/
    ├── main.js         # Vue app mount
    └── App.vue         # Dashboard component (template + script + styles)
```

See [FRONTEND_SETUP.md](../FRONTEND_SETUP.md) for the full project setup guide.
