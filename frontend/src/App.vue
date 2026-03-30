<template>
  <div class="dashboard">
    <header class="header">
      <h1>💧 Water System Control Panel</h1>
      <div class="connection-status" :class="connectionStatus">
        <span class="dot"></span>
        {{ connectionStatus === 'connected' ? 'Connected' : 'Disconnected' }}
      </div>
    </header>

    <div class="connection-config">
      <div class="config-group">
        <label>Broker URL (WebSocket)</label>
        <input v-model="brokerUrl" placeholder="wss://your-vm-ip:8884" @keyup.enter="connect">
        <button @click="connect" :disabled="connectionStatus === 'connected'">
          {{ connectionStatus === 'connected' ? 'Connected' : 'Connect' }}
        </button>
      </div>
    </div>

    <div class="grid">
      <!-- Device Monitoring -->
      <div class="card">
        <h2>📊 Sensor Readings</h2>
        <div class="device-list">
          <div v-if="Object.keys(devices).length === 0" class="empty-state">
            No devices connected yet...
          </div>
          <div v-for="(device, deviceId) in devices" :key="deviceId" class="device-item">
            <div class="device-header">
              <h3>{{ deviceId }}</h3>
              <span class="device-status" :class="device.connected ? 'online' : 'offline'">
                {{ device.connected ? 'Online' : 'Offline' }}
              </span>
            </div>
            
            <div class="reading water-level">
              <span class="label">Water Level</span>
              <div class="value-display">
                <span class="number">{{ device.waterLevel }}</span>
                <div class="bar">
                  <div class="fill" :style="{ width: Math.min((device.waterLevel / 2048) * 100, 100) + '%' }"></div>
                </div>
              </div>
              <span class="threshold" :class="getWaterLevelStatus(device.waterLevel)">
                {{ getWaterLevelStatus(device.waterLevel) === 'high' ? '⚠️ HIGH' : '✓ Normal' }}
              </span>
            </div>

            <div class="reading raindrop">
              <span class="label">Raindrop Sensor</span>
              <div class="value-display">
                <span class="badge" :class="device.raindrop ? 'detected' : 'clear'">
                  {{ device.raindrop ? '💧 Detected' : '☀️ Clear' }}
                </span>
              </div>
            </div>

            <div class="actuation-section">
              <div class="status">
                <span class="label">Pump Status</span>
                <span class="badge" :class="device.pumpActive ? 'active' : 'inactive'">
                  {{ device.pumpActive ? '🔴 ACTIVE' : '⚪ Inactive' }}
                </span>
              </div>
              <button 
                @click="togglePump(deviceId)" 
                class="btn-toggle"
                :class="device.pumpActive ? 'btn-deactivate' : 'btn-activate'"
              >
                {{ device.pumpActive ? 'Deactivate Pump' : 'Activate Pump' }}
              </button>
            </div>

            <div class="timestamps">
              <small>Last update: {{ formatTime(device.lastUpdate) }}</small>
            </div>
          </div>
        </div>
      </div>

      <!-- System Status -->
      <div class="card">
        <h2>📡 System Status</h2>
        <div class="status-info">
          <div class="stat">
            <span class="stat-label">Connected Devices</span>
            <span class="stat-value">{{ Object.keys(devices).length }}</span>
          </div>
          <div class="stat">
            <span class="stat-label">Active Pumps</span>
            <span class="stat-value">{{ activePumpCount }}</span>
          </div>
          <div class="stat">
            <span class="stat-label">High Water Alerts</span>
            <span class="stat-value alert">{{ highWaterCount }}</span>
          </div>
        </div>

        <div class="broker-info">
          <h3>Broker Info</h3>
          <input v-model="brokerUrl" placeholder="wss://ip:port" class="broker-input">
          <p class="hint">💡 For SSL: use <code>wss://</code> | For WebSocket: use <code>ws://</code></p>
        </div>
      </div>
    </div>

    <!-- Error/Info Messages -->
    <div v-if="errorMessage" class="alert alert-error">
      {{ errorMessage }}
      <button @click="errorMessage = ''" class="close-btn">×</button>
    </div>
  </div>
</template>

<script>
import { reactive, computed } from 'vue'
import mqtt from 'mqtt'

export default {
  name: 'App',
  setup() {
    const state = reactive({
      brokerUrl: 'ws://150.230.122.17:8884',
      connectionStatus: 'disconnected',
      devices: {},
      client: null,
      errorMessage: ''
    })

    const connect = () => {
      try {
        state.errorMessage = ''
        
        if (state.client) {
          state.client.end()
        }

        const options = {
          clientId: 'web-dashboard-' + Math.random().toString(16).substr(2, 8),
          clean: true,
          keepalive: 60,
          reconnectPeriod: 1000,
          rejectUnauthorized: false
        }

        state.client = mqtt.connect(state.brokerUrl, options)

        state.client.on('connect', () => {
          state.connectionStatus = 'connected'
          console.log('Connected to MQTT broker')
          
          // Subscribe to all sensor topics
          state.client.subscribe('devices/+/water_level', { qos: 1 })
          state.client.subscribe('devices/+/actuate', { qos: 1 })
        })

        state.client.on('message', (topic, message) => {
          handleMqttMessage(topic, message.toString())
        })

        state.client.on('disconnect', () => {
          state.connectionStatus = 'disconnected'
        })

        state.client.on('error', (error) => {
          state.errorMessage = `Connection error: ${error.message}`
          state.connectionStatus = 'error'
        })

      } catch (error) {
        state.errorMessage = `Failed to connect: ${error.message}`
      }
    }

    const handleMqttMessage = (topic, payload) => {
      try {
        const parts = topic.split('/')
        const deviceId = parts[1]

        if (!state.devices[deviceId]) {
          state.devices[deviceId] = {
            waterLevel: 0,
            raindrop: false,
            pumpActive: false,
            connected: true,
            lastUpdate: Date.now()
          }
        }

        if (topic.includes('water_level')) {
          const data = JSON.parse(payload)
          state.devices[deviceId].waterLevel = data.water_sensor || data.water_level || 0
          state.devices[deviceId].raindrop = data.raindrop_sensor?.digital === 0 || data.raindrop_sensor?.analog > 1500
          state.devices[deviceId].connected = true
          state.devices[deviceId].lastUpdate = Date.now()
        } else if (topic.includes('actuate')) {
          const data = JSON.parse(payload)
          state.devices[deviceId].pumpActive = data.action === 'activate'
          state.devices[deviceId].lastUpdate = Date.now()
        }
      } catch (error) {
        console.error('Failed to parse message:', error)
      }
    }

    const togglePump = (deviceId) => {
      if (!state.client || state.connectionStatus !== 'connected') {
        state.errorMessage = 'Not connected to broker'
        return
      }

      const device = state.devices[deviceId]
      const action = device.pumpActive ? 'deactivate' : 'activate'
      const topic = `devices/${deviceId}/actuate`
      const payload = JSON.stringify({ action })

      state.client.publish(topic, payload, { qos: 1 }, (err) => {
        if (err) {
          state.errorMessage = `Failed to send command: ${err.message}`
        }
      })
    }

    const getWaterLevelStatus = (level) => {
      return level > 1900 ? 'high' : 'normal'
    }

    const formatTime = (timestamp) => {
      const now = Date.now()
      const diff = (now - timestamp) / 1000
      if (diff < 60) return 'just now'
      if (diff < 3600) return Math.floor(diff / 60) + 'm ago'
      return Math.floor(diff / 3600) + 'h ago'
    }

    const activePumpCount = computed(() => {
      return Object.values(state.devices).filter(d => d.pumpActive).length
    })

    const highWaterCount = computed(() => {
      return Object.values(state.devices).filter(d => d.waterLevel > 1900).length
    })

    return {
      ...state,
      connect,
      togglePump,
      getWaterLevelStatus,
      formatTime,
      activePumpCount,
      highWaterCount
    }
  }
}
</script>

<style scoped>
.dashboard {
  padding: 20px;
}

.header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 30px;
  color: white;
}

.header h1 {
  font-size: 2.5em;
}

.connection-status {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 10px 20px;
  background: rgba(255, 255, 255, 0.1);
  border-radius: 20px;
  font-weight: 600;
}

.connection-status .dot {
  width: 10px;
  height: 10px;
  border-radius: 50%;
  background: #ff6b6b;
  animation: blink 1s infinite;
}

.connection-status.connected .dot {
  background: #51cf66;
  animation: none;
}

@keyframes blink {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.5; }
}

.connection-config {
  margin-bottom: 30px;
}

.config-group {
  display: flex;
  gap: 10px;
  align-items: center;
  background: rgba(255, 255, 255, 0.1);
  padding: 15px;
  border-radius: 10px;
}

.config-group label {
  color: white;
  font-weight: 500;
  white-space: nowrap;
}

.config-group input {
  flex: 1;
  padding: 8px 12px;
  border: none;
  border-radius: 5px;
  font-family: monospace;
}

.config-group button {
  padding: 8px 16px;
  background: #4dabf7;
  color: white;
  border: none;
  border-radius: 5px;
  cursor: pointer;
  font-weight: 600;
  transition: background 0.3s;
}

.config-group button:hover:not(:disabled) {
  background: #339af0;
}

.config-group button:disabled {
  background: #51cf66;
  cursor: default;
}

.grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(500px, 1fr));
  gap: 20px;
  margin-bottom: 20px;
}

.card {
  background: white;
  border-radius: 12px;
  padding: 20px;
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.15);
}

.card h2 {
  margin-bottom: 20px;
  color: #1a1a1a;
  border-bottom: 3px solid #4dabf7;
  padding-bottom: 10px;
}

.device-list {
  display: flex;
  flex-direction: column;
  gap: 15px;
}

.empty-state {
  text-align: center;
  padding: 40px 20px;
  color: #999;
  font-style: italic;
}

.device-item {
  border: 2px solid #e9ecef;
  border-radius: 10px;
  padding: 15px;
  background: #f8f9fa;
  transition: border-color 0.3s;
}

.device-item:hover {
  border-color: #4dabf7;
}

.device-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 15px;
}

.device-header h3 {
  margin: 0;
  color: #1a1a1a;
}

.device-status {
  padding: 4px 12px;
  border-radius: 20px;
  font-size: 0.85em;
  font-weight: 600;
}

.device-status.online {
  background: #d3f9d8;
  color: #2f5233;
}

.device-status.offline {
  background: #ffe3e3;
  color: #5c2e2e;
}

.reading {
  margin-bottom: 15px;
  padding: 10px;
  background: white;
  border-radius: 8px;
}

.reading .label {
  display: block;
  font-weight: 600;
  color: #666;
  margin-bottom: 8px;
  font-size: 0.9em;
}

.value-display {
  display: flex;
  align-items: center;
  gap: 10px;
  margin-bottom: 8px;
}

.reading.water-level .number {
  font-size: 1.8em;
  font-weight: bold;
  color: #2196f3;
  min-width: 80px;
}

.bar {
  flex: 1;
  height: 24px;
  background: #e9ecef;
  border-radius: 12px;
  overflow: hidden;
}

.bar .fill {
  height: 100%;
  background: linear-gradient(90deg, #2196f3, #1976d2);
  transition: width 0.3s ease;
}

.threshold {
  display: block;
  font-size: 0.85em;
  font-weight: 600;
  margin-top: 5px;
}

.threshold.high {
  color: #e03131;
}

.threshold.normal {
  color: #51cf66;
}

.badge {
  display: inline-block;
  padding: 6px 12px;
  border-radius: 20px;
  font-weight: 600;
  font-size: 0.9em;
}

.badge.detected {
  background: #a5d8ff;
  color: #0c5aa0;
}

.badge.clear {
  background: #c6f6d5;
  color: #22863a;
}

.badge.active {
  background: #ffa8a8;
  color: #a61e4d;
  animation: pulse 1.5s infinite;
}

.badge.inactive {
  background: #e9ecef;
  color: #495057;
}

@keyframes pulse {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.7; }
}

.actuation-section {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 12px;
  background: white;
  border-radius: 8px;
  margin-bottom: 10px;
}

.actuation-section .status {
  display: flex;
  flex-direction: column;
  gap: 5px;
}

.actuation-section .label {
  font-weight: 600;
  color: #666;
  font-size: 0.9em;
}

.btn-toggle {
  padding: 8px 16px;
  border: 2px solid;
  border-radius: 6px;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.3s;
}

.btn-activate {
  border-color: #51cf66;
  background: #d3f9d8;
  color: #37b24d;
}

.btn-activate:hover {
  background: #51cf66;
  color: white;
}

.btn-deactivate {
  border-color: #ff8787;
  background: #ffe3e3;
  color: #e03131;
}

.btn-deactivate:hover {
  background: #ff8787;
  color: white;
}

.timestamps {
  text-align: right;
  color: #999;
}

.status-info {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(140px, 1fr));
  gap: 15px;
  margin-bottom: 25px;
}

.stat {
  background: #f8f9fa;
  padding: 15px;
  border-radius: 8px;
  border-left: 4px solid #4dabf7;
}

.stat-label {
  display: block;
  color: #666;
  font-size: 0.9em;
  margin-bottom: 8px;
}

.stat-value {
  display: block;
  font-size: 2em;
  font-weight: bold;
  color: #2196f3;
}

.stat-value.alert {
  color: #e03131;
}

.broker-info {
  background: #f8f9fa;
  padding: 15px;
  border-radius: 8px;
}

.broker-info h3 {
  margin: 0 0 10px 0;
  font-size: 0.95em;
  color: #333;
}

.broker-input {
  width: 100%;
  padding: 8px;
  border: 1px solid #ddd;
  border-radius: 5px;
  font-family: monospace;
  font-size: 0.85em;
  margin-bottom: 10px;
}

.hint {
  font-size: 0.85em;
  color: #666;
  margin: 0;
}

.hint code {
  background: #e9ecef;
  padding: 2px 6px;
  border-radius: 3px;
  font-family: monospace;
}

.alert {
  padding: 15px;
  border-radius: 8px;
  margin-bottom: 20px;
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.alert-error {
  background: #ffe3e3;
  color: #a61e4d;
  border-left: 4px solid #e03131;
}

.close-btn {
  background: none;
  border: none;
  color: inherit;
  font-size: 1.5em;
  cursor: pointer;
  padding: 0;
}
</style>
