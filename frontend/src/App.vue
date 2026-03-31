<template>
  <div class="dashboard">
    <header class="header">
      <h1>Water System Control Panel</h1>
      <div class="connection-badge" :class="state.connectionStatus">
        <span class="dot"></span>
        {{ statusLabel }}
      </div>
    </header>

    <!-- Connection Bar -->
    <div class="connection-bar">
      <label>Broker</label>
      <input
        v-model="state.brokerUrl"
        placeholder="ws://localhost:8884"
        :disabled="state.connectionStatus === 'connected'"
        @keyup.enter="connect"
      >
      <button v-if="state.connectionStatus !== 'connected'" class="btn btn-connect" @click="connect">
        Connect
      </button>
      <button v-else class="btn btn-disconnect" @click="disconnect">
        Disconnect
      </button>
    </div>

    <!-- Error Banner -->
    <div v-if="state.errorMessage" class="banner banner-error">
      {{ state.errorMessage }}
      <button class="banner-close" @click="state.errorMessage = ''">×</button>
    </div>

    <!-- Stats Row -->
    <div class="stats-row">
      <div class="stat-card">
        <span class="stat-number">{{ deviceCount }}</span>
        <span class="stat-label">Devices</span>
      </div>
      <div class="stat-card">
        <span class="stat-number">{{ activeActuatorCount }}</span>
        <span class="stat-label">Active Actuators</span>
      </div>
      <div class="stat-card" :class="{ 'stat-alert': highWaterCount > 0 }">
        <span class="stat-number">{{ highWaterCount }}</span>
        <span class="stat-label">High Water Alerts</span>
      </div>
      <div class="stat-card">
        <span class="stat-number">{{ state.messageCount }}</span>
        <span class="stat-label">Messages</span>
      </div>
    </div>

    <!-- Main Content -->
    <div class="main-grid">
      <!-- Devices -->
      <div class="panel">
        <h2>Sensor Readings</h2>
        <div v-if="deviceCount === 0" class="empty-state">
          <p v-if="state.connectionStatus !== 'connected'">Connect to the broker to see devices.</p>
          <p v-else>Waiting for device data...</p>
        </div>
        <div v-for="(device, deviceId) in state.devices" :key="deviceId" class="device-card">
          <div class="device-top">
            <h3>{{ deviceId }}</h3>
            <span class="status-pill online">Online</span>
          </div>

          <!-- Water Level -->
          <div class="sensor-row">
            <span class="sensor-label">Water Level</span>
            <span class="sensor-value" :class="{ 'value-high': device.waterLevel > 1900 }">
              {{ device.waterLevel }}
            </span>
          </div>
          <div class="level-bar">
            <div
              class="level-fill"
              :class="{ 'fill-high': device.waterLevel > 1900, 'fill-warn': device.waterLevel > 1880 && device.waterLevel <= 1900 }"
              :style="{ width: Math.min((device.waterLevel / 2048) * 100, 100) + '%' }"
            ></div>
          </div>
          <div class="sensor-row">
            <span class="sensor-hint">Threshold: 1900 (on) / 1880 (off)</span>
            <span class="sensor-tag" :class="device.waterLevel > 1900 ? 'tag-high' : 'tag-normal'">
              {{ device.waterLevel > 1900 ? 'HIGH' : 'Normal' }}
            </span>
          </div>

          <!-- Raindrop -->
          <div class="sensor-row">
            <span class="sensor-label">Raindrop</span>
            <span class="sensor-tag" :class="device.raindrop ? 'tag-rain' : 'tag-clear'">
              {{ device.raindrop ? 'Detected' : 'Clear' }}
            </span>
          </div>
          <div v-if="device.raindropAnalog != null" class="sensor-row">
            <span class="sensor-hint">Analog: {{ device.raindropAnalog }}</span>
            <span class="sensor-hint">Digital: {{ device.raindropDigital }}</span>
          </div>

          <!-- Actuator -->
          <div class="pump-row">
            <div>
              <span class="sensor-label">Actuators</span>
              <span class="pump-status" :class="device.pumpActive ? 'pump-on' : 'pump-off'">
                {{ device.pumpActive ? 'ACTIVE' : 'Inactive' }}
              </span>
              <span v-if="device.pumpReason" class="pump-reason">{{ device.pumpReason }}</span>
            </div>
            <button
              class="btn btn-pump"
              :class="device.pumpActive ? 'btn-pump-off' : 'btn-pump-on'"
              :disabled="state.connectionStatus !== 'connected'"
              @click="togglePump(deviceId)"
            >
              {{ device.pumpActive ? 'Deactivate' : 'Activate' }}
            </button>
          </div>

          <div class="device-footer">
            Last update: {{ formatTime(device.lastUpdate) }}
          </div>
        </div>
      </div>

      <!-- Activity Log -->
      <div class="panel panel-log">
        <div class="log-header">
          <h2>Activity Log</h2>
          <button class="btn btn-small" @click="state.activityLog = []">Clear</button>
        </div>
        <div class="log-list" ref="logList">
          <div v-if="state.activityLog.length === 0" class="empty-state">
            No activity yet.
          </div>
          <div v-for="(entry, i) in state.activityLog" :key="i" class="log-entry" :class="'log-' + entry.type">
            <span class="log-time">{{ entry.time }}</span>
            <span class="log-msg">{{ entry.message }}</span>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script>
import { reactive, computed, onUnmounted, nextTick, ref } from 'vue'
import mqtt from 'mqtt'

const WATER_ON_THRESHOLD = 1500
const RAINDROP_ANALOG_THRESHOLD = 1500
const MAX_LOG_ENTRIES = 100

export default {
  name: 'App',
  setup() {
    const logList = ref(null)
    let mqttClient = null

    const state = reactive({
      brokerUrl: 'ws://localhost:18884',
      connectionStatus: 'disconnected',
      devices: {},
      errorMessage: '',
      messageCount: 0,
      activityLog: []
    })

    const addLog = (type, message) => {
      const now = new Date()
      const time = now.toLocaleTimeString('en-GB', { hour12: false })
      state.activityLog.unshift({ type, message, time })
      if (state.activityLog.length > MAX_LOG_ENTRIES) {
        state.activityLog.length = MAX_LOG_ENTRIES
      }
    }

    const connect = () => {
      state.errorMessage = ''

      if (mqttClient) {
        mqttClient.end(true)
        mqttClient = null
      }

      const url = state.brokerUrl.trim()
      if (!url) {
        state.errorMessage = 'Enter a broker URL'
        return
      }

      addLog('info', `Connecting to ${url}...`)

      mqttClient = mqtt.connect(url, {
        clientId: 'web-dashboard-' + Math.random().toString(16).slice(2, 10),
        clean: true,
        keepalive: 60,
        reconnectPeriod: 3000,
        connectTimeout: 10000,
        rejectUnauthorized: false
      })

      mqttClient.on('connect', () => {
        state.connectionStatus = 'connected'
        state.errorMessage = ''
        addLog('success', 'Connected to broker')
        mqttClient.subscribe('devices/+/water_level', { qos: 1 })
        mqttClient.subscribe('devices/+/actuate', { qos: 1 })
        addLog('info', 'Subscribed to devices/+/water_level and devices/+/actuate')
      })

      mqttClient.on('message', (topic, message) => {
        state.messageCount++
        handleMessage(topic, message.toString())
      })

      mqttClient.on('reconnect', () => {
        state.connectionStatus = 'reconnecting'
        addLog('warn', 'Reconnecting...')
      })

      mqttClient.on('close', () => {
        state.connectionStatus = 'disconnected'
      })

      mqttClient.on('offline', () => {
        state.connectionStatus = 'disconnected'
        addLog('warn', 'Broker offline')
      })

      mqttClient.on('error', (err) => {
        state.errorMessage = err.message
        addLog('error', `Error: ${err.message}`)
      })
    }

    const disconnect = () => {
      if (mqttClient) {
        mqttClient.end(true)
        mqttClient = null
      }
      state.connectionStatus = 'disconnected'
      addLog('info', 'Disconnected')
    }

    const handleMessage = (topic, payload) => {
      try {
        const parts = topic.split('/')
        const deviceId = parts[1]
        const msgType = parts[2]

        if (!state.devices[deviceId]) {
          state.devices[deviceId] = {
            waterLevel: 0,
            raindrop: false,
            raindropAnalog: null,
            raindropDigital: null,
            pumpActive: false,
            pumpReason: '',
            lastUpdate: Date.now()
          }
          addLog('info', `New device: ${deviceId}`)
        }

        const data = JSON.parse(payload)

        if (msgType === 'water_level') {
          const level = data.water_level != null ? data.water_level : (data.water_sensor != null ? data.water_sensor : 0)
          state.devices[deviceId].waterLevel = level
          state.devices[deviceId].lastUpdate = Date.now()

          const rd = data.raindrop_sensor
          if (rd) {
            state.devices[deviceId].raindropAnalog = rd.analog ?? null
            state.devices[deviceId].raindropDigital = rd.digital ?? null
            const digitalOn = rd.digital != null && (String(rd.digital) === '1' || String(rd.digital) === 'true')
            const analogOn = rd.analog != null && Number(rd.analog) >= RAINDROP_ANALOG_THRESHOLD
            state.devices[deviceId].raindrop = digitalOn || analogOn
          }

          const tag = level > WATER_ON_THRESHOLD ? ' [HIGH]' : ''
          addLog('data', `${deviceId} water=${level}${tag}`)

        } else if (msgType === 'actuate') {
          // New format with pump, servo, etc.
          state.devices[deviceId].pumpActive = data.pump === 1
          state.devices[deviceId].pumpReason = data.reason || ''
          state.devices[deviceId].lastUpdate = Date.now()

          const action = data.pump === 1 ? 'ON' : 'OFF'
          addLog(data.pump === 1 ? 'warn' : 'success', `${deviceId} actuator ${action}: ${data.reason || ''}`)
        }
      } catch (err) {
        addLog('error', `Parse error: ${err.message}`)
      }
    }

    const togglePump = (deviceId) => {
      if (!mqttClient || state.connectionStatus !== 'connected') {
        state.errorMessage = 'Not connected to broker'
        return
      }
      const device = state.devices[deviceId]
      const action = device.pumpActive ? 'deactivate' : 'activate'
      mqttClient.publish(
        `devices/${deviceId}/actuate`,
        JSON.stringify({ action }),
        { qos: 1 },
        (err) => {
          if (err) {
            state.errorMessage = `Publish failed: ${err.message}`
          } else {
            addLog('info', `Manual actuator ${action} sent to ${deviceId}`)
          }
        }
      )
    }

    const formatTime = (ts) => {
      if (!ts) return 'never'
      const diff = (Date.now() - ts) / 1000
      if (diff < 5) return 'just now'
      if (diff < 60) return Math.floor(diff) + 's ago'
      if (diff < 3600) return Math.floor(diff / 60) + 'm ago'
      return Math.floor(diff / 3600) + 'h ago'
    }

    const statusLabel = computed(() => {
      const map = { connected: 'Connected', disconnected: 'Disconnected', reconnecting: 'Reconnecting...' }
      return map[state.connectionStatus] || 'Disconnected'
    })

    const deviceCount = computed(() => Object.keys(state.devices).length)
    const activeActuatorCount = computed(() => Object.values(state.devices).filter(d => d.pumpActive).length)
    const highWaterCount = computed(() => Object.values(state.devices).filter(d => d.waterLevel > WATER_ON_THRESHOLD).length)

    onUnmounted(() => {
      if (mqttClient) {
        mqttClient.end(true)
      }
    })

    return {
      state,
      logList,
      connect,
      disconnect,
      togglePump,
      formatTime,
      statusLabel,
      deviceCount,
      activeActuatorCount,
      highWaterCount
    }
  }
}
</script>

<style scoped>
.dashboard {
  padding: 20px;
  color: #e0e0e0;
}

/* Header */
.header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 20px;
}

.header h1 {
  font-size: 1.8em;
  color: #fff;
}

.connection-badge {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 16px;
  border-radius: 20px;
  font-weight: 600;
  font-size: 0.9em;
  background: rgba(255,255,255,0.08);
  color: #ccc;
}

.connection-badge .dot {
  width: 10px;
  height: 10px;
  border-radius: 50%;
  background: #ff6b6b;
}

.connection-badge.connected { color: #51cf66; }
.connection-badge.connected .dot { background: #51cf66; }
.connection-badge.reconnecting { color: #ffd43b; }
.connection-badge.reconnecting .dot { background: #ffd43b; animation: pulse 1s infinite; }

@keyframes pulse {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.3; }
}

/* Connection Bar */
.connection-bar {
  display: flex;
  gap: 10px;
  align-items: center;
  background: rgba(255,255,255,0.06);
  padding: 12px 16px;
  border-radius: 10px;
  margin-bottom: 16px;
}

.connection-bar label {
  font-weight: 600;
  font-size: 0.9em;
  white-space: nowrap;
}

.connection-bar input {
  flex: 1;
  padding: 8px 12px;
  border: 1px solid rgba(255,255,255,0.15);
  border-radius: 6px;
  background: rgba(0,0,0,0.2);
  color: #fff;
  font-family: monospace;
  font-size: 0.95em;
}

.connection-bar input:disabled {
  opacity: 0.5;
}

/* Buttons */
.btn {
  padding: 8px 18px;
  border: none;
  border-radius: 6px;
  cursor: pointer;
  font-weight: 600;
  font-size: 0.9em;
  transition: background 0.2s, opacity 0.2s;
}

.btn:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}

.btn-connect { background: #339af0; color: #fff; }
.btn-connect:hover { background: #228be6; }
.btn-disconnect { background: #e03131; color: #fff; }
.btn-disconnect:hover { background: #c92a2a; }
.btn-small { padding: 4px 10px; font-size: 0.8em; background: rgba(255,255,255,0.1); color: #ccc; }
.btn-small:hover { background: rgba(255,255,255,0.2); }

/* Banner */
.banner {
  padding: 10px 16px;
  border-radius: 8px;
  margin-bottom: 16px;
  display: flex;
  justify-content: space-between;
  align-items: center;
  font-size: 0.9em;
}

.banner-error {
  background: rgba(224, 49, 49, 0.15);
  border: 1px solid rgba(224, 49, 49, 0.3);
  color: #ffa8a8;
}

.banner-close {
  background: none;
  border: none;
  color: inherit;
  font-size: 1.3em;
  cursor: pointer;
  padding: 0 4px;
}

/* Stats Row */
.stats-row {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 12px;
  margin-bottom: 20px;
}

.stat-card {
  background: rgba(255,255,255,0.06);
  border-radius: 10px;
  padding: 16px;
  text-align: center;
}

.stat-card.stat-alert {
  background: rgba(224, 49, 49, 0.12);
  border: 1px solid rgba(224, 49, 49, 0.25);
}

.stat-number {
  display: block;
  font-size: 2em;
  font-weight: 700;
  color: #fff;
}

.stat-alert .stat-number {
  color: #ff6b6b;
}

.stat-label {
  font-size: 0.8em;
  color: #aaa;
  text-transform: uppercase;
  letter-spacing: 0.5px;
}

/* Main Grid */
.main-grid {
  display: grid;
  grid-template-columns: 1fr 380px;
  gap: 20px;
}

@media (max-width: 900px) {
  .main-grid { grid-template-columns: 1fr; }
  .stats-row { grid-template-columns: repeat(2, 1fr); }
}

/* Panels */
.panel {
  background: rgba(255,255,255,0.05);
  border-radius: 12px;
  padding: 20px;
}

.panel h2 {
  margin: 0 0 16px 0;
  font-size: 1.2em;
  color: #fff;
  border-bottom: 2px solid rgba(77, 171, 247, 0.4);
  padding-bottom: 10px;
}

/* Empty State */
.empty-state {
  text-align: center;
  padding: 40px 20px;
  color: #888;
}

/* Device Card */
.device-card {
  background: rgba(0,0,0,0.2);
  border: 1px solid rgba(255,255,255,0.08);
  border-radius: 10px;
  padding: 16px;
  margin-bottom: 12px;
}

.device-card:last-child {
  margin-bottom: 0;
}

.device-top {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 12px;
}

.device-top h3 {
  margin: 0;
  font-size: 1.1em;
  color: #fff;
}

.status-pill {
  padding: 3px 10px;
  border-radius: 12px;
  font-size: 0.75em;
  font-weight: 600;
}

.status-pill.online {
  background: rgba(81, 207, 102, 0.15);
  color: #51cf66;
}

/* Sensor Rows */
.sensor-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 4px 0;
}

.sensor-label {
  font-weight: 600;
  font-size: 0.9em;
  color: #ccc;
}

.sensor-value {
  font-size: 1.6em;
  font-weight: 700;
  color: #4dabf7;
}

.sensor-value.value-high {
  color: #ff6b6b;
}

.sensor-hint {
  font-size: 0.75em;
  color: #888;
}

.sensor-tag {
  padding: 2px 10px;
  border-radius: 10px;
  font-size: 0.75em;
  font-weight: 600;
}

.tag-normal { background: rgba(81,207,102,0.15); color: #51cf66; }
.tag-high { background: rgba(255,107,107,0.15); color: #ff6b6b; }
.tag-rain { background: rgba(77,171,247,0.15); color: #4dabf7; }
.tag-clear { background: rgba(255,255,255,0.08); color: #aaa; }

/* Level Bar */
.level-bar {
  height: 8px;
  background: rgba(255,255,255,0.08);
  border-radius: 4px;
  overflow: hidden;
  margin: 6px 0;
}

.level-fill {
  height: 100%;
  border-radius: 4px;
  background: #4dabf7;
  transition: width 0.4s ease;
}

.level-fill.fill-warn { background: #ffd43b; }
.level-fill.fill-high { background: #ff6b6b; }

/* Pump Row */
.pump-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-top: 10px;
  padding-top: 10px;
  border-top: 1px solid rgba(255,255,255,0.06);
}

.pump-status {
  font-weight: 700;
  font-size: 0.9em;
  margin-left: 10px;
}

.pump-on { color: #ff6b6b; }
.pump-off { color: #888; }

.pump-reason {
  display: block;
  font-size: 0.7em;
  color: #888;
  margin-top: 2px;
}

.btn-pump {
  padding: 6px 14px;
  font-size: 0.85em;
}

.btn-pump-on { background: #e03131; color: #fff; }
.btn-pump-on:hover { background: #c92a2a; }
.btn-pump-off { background: #495057; color: #fff; }
.btn-pump-off:hover { background: #343a40; }

/* Device Footer */
.device-footer {
  margin-top: 8px;
  font-size: 0.75em;
  color: #666;
  text-align: right;
}

/* Activity Log */
.panel-log {
  display: flex;
  flex-direction: column;
  max-height: 600px;
}

.log-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 12px;
}

.log-header h2 {
  margin: 0;
  border: none;
  padding: 0;
}

.log-list {
  flex: 1;
  overflow-y: auto;
  font-family: monospace;
  font-size: 0.8em;
}

.log-entry {
  padding: 4px 8px;
  border-radius: 4px;
  margin-bottom: 2px;
  display: flex;
  gap: 8px;
}

.log-time {
  color: #666;
  white-space: nowrap;
}

.log-msg {
  word-break: break-word;
}

.log-info { color: #aaa; }
.log-data { color: #4dabf7; }
.log-success { color: #51cf66; }
.log-warn { color: #ffd43b; }
.log-error { color: #ff6b6b; }
</style>
