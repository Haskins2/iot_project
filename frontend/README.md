# Water System UI

Vue.js web dashboard for real-time monitoring and control of the water level actuation system.

## Features

- 📊 Real-time sensor readings (water level, raindrop detection)
- 🚰 Live pump status monitoring
- 📡 Device connection status
- 🎮 Manual pump control
- 📈 System statistics (connected devices, active pumps, alerts)

## Setup

### Prerequisites

- Node.js 16+ and npm
- Access to the MQTT broker (WebSocket enabled on port 8884)

### Installation

```bash
cd frontend
npm install
```

### Development Server

```bash
npm run dev
```

The dashboard will be available at `http://localhost:5173`

### Production Build

```bash
npm run build
npm run preview
```

## Browser Access

1. Open your browser to the development server URL
2. Update the broker URL if needed (default: `ws://150.230.122.17:8884`)
3. Click "Connect" to start receiving real-time updates

## Broker URL Format

- **WebSocket (unencrypted):** `ws://ip-address:8884`
- **WebSocket over TLS:** `wss://ip-address:8884`

## Connection Requirements

- The MQTT broker must have WebSocket listener enabled (port 8884)
- Browser CORS allows WebSocket connections
- For WSSL connections, browser must trust the broker's certificate

## Architecture

The dashboard uses:
- **Vue 3** for the UI framework
- **Vite** for build tooling
- **MQTT.js** for WebSocket MQTT connections
- **Real-time updates** via MQTT pub/sub

Topics monitored:
- `devices/+/water_level` - Sensor telemetry
- `devices/+/actuate` - Pump control status

## Docker Deployment

To run the frontend in a Docker container:

```bash
docker build -f frontend.Dockerfile -t water-ui .
docker run -p 5173:3000 water-ui
```

Connect to `http://localhost:5173` from your browser.
