# 🚀 Water System UI - Quick Start Guide

Your Vue.js web dashboard is ready! Here's how to get it running:

## Step 1: Update and Rebuild Backend

First, redeploy the backend with WebSocket enabled:

```bash
cd ~/iot_project/backend

# Update the broker config and rebuild
docker compose down
docker compose up -d --build
```

This enables the WebSocket listener on port 8884 that the UI needs.

## Step 2: Install and Run Frontend (Local Development)

On your local machine:

```bash
cd frontend
npm install
npm run dev
```

Your dashboard will open at **http://localhost:5173**

## Step 3: Connect to Your Broker

In the dashboard:
1. Update the broker URL if needed: `ws://150.230.122.17:8884`
2. Click the "Connect" button
3. You should see real-time sensor data appear

## Dashboard Features

✅ **Real-Time Monitoring**
- Water level readings with visual bar chart
- Raindrop sensor status
- Device connectivity status
- Last update timestamps

✅ **Pump Control**
- Manual activate/deactivate buttons
- Live pump status indicator
- System-wide statistics (active pumps, alerts)

✅ **Connection Management**
- Visual connection status indicator
- Error messages for troubleshooting
- Quick reconnect button

## Broker URL Options

| URL Format | Type | Use Case |
|-----------|------|----------|
| `ws://150.230.122.17:8884` | WebSocket (unencrypted) | Development/LAN |
| `wss://150.230.122.17:8884` | WebSocket over TLS | Remote/Public access |

## Testing the Flow

### Test 1: Send a High Water Level Message
```bash
docker compose exec -T mosquitto mosquitto_pub \
  -h localhost -p 8883 \
  --cafile /mosquitto/certs/ca.crt \
  --cert /mosquitto/certs/esp32-master-01.crt \
  --key /mosquitto/certs/esp32-master-01.key \
  -t devices/esp32-master-01/water_level \
  -m '{"water_sensor":2100,"raindrop_sensor":{"digital":0}}'
```

Expected: Dashboard shows HIGH water level indicator ⚠️

### Test 2: Manual Pump Control
Click "Activate Pump" in the dashboard → Check broker logs to confirm message sent

### Test 3: Spike Detection
Send 5 rapid low→high→low messages to verify spike rejection (3-second persistence requirement)

## Troubleshooting

**"Connection failed"**
- Check broker is running: `docker compose ps`
- Verify port 8884 is exposed: `docker compose ps mosquitto`
- Check firewall allows WebSocket connections

**"No devices appearing"**
- Publish a test message using the commands above
- Check broker logs: `docker compose logs mosquitto`
- Verify ACL permissions: `cat backend/mosquitto/config/acl`

**"Sensor readings not updating"**
- Check browser console for errors (F12)
- Verify MQTT subscription: `docker compose logs broker | grep "devices/"`
- Try reconnecting to the broker

## Production Deployment

To run the frontend in Docker alongside your backend:

```bash
cd ~/iot_project
docker build -f frontend.Dockerfile -t water-ui .
docker run -p 5173:3000 --network mqtt-network water-ui
```

Or add to docker-compose.yml:
```yaml
frontend:
  image: water-ui
  ports:
    - "5173:3000"
  depends_on:
    - mosquitto
```

## File Structure

```
frontend/
├── index.html          # Entry point
├── package.json        # Dependencies
├── vite.config.js      # Vite configuration
├── README.md           # Docs
└── src/
    ├── main.js         # Vue app setup
    └── App.vue         # Main dashboard component
```

## Next Steps

- 🎨 Customize colors/styling in `App.vue`
- 📊 Add historical data graphs
- 🔔 Add audio/visual alerts for high water
- 📱 Improve mobile responsiveness
- 🔐 Add user authentication

Enjoy your dashboard! 🎉
