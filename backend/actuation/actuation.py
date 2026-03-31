#!/usr/bin/env python3
"""
Actuation Logic Service
Subscribes to devices/+/water_level, analyses the payload,
and publishes a command to devices/{device_id}/actuate.
"""

import os
import sys
import ssl
import time
import json
import logging
from collections import defaultdict, deque
from datetime import datetime, timezone
import paho.mqtt.client as mqtt

# Logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[logging.StreamHandler(sys.stdout)]
)
logger = logging.getLogger('actuation-service')

# ENV based config
MQTT_BROKER = os.getenv('MQTT_BROKER', 'mosquitto')
MQTT_PORT = int(os.getenv('MQTT_PORT', '8883'))
# TODO Add additional topics
MQTT_TOPICS = os.getenv('MQTT_TOPICS', 'devices/+/water_level').split(',')
CA_CERT = os.getenv('CA_CERT', '/certs/ca.crt')
CLIENT_CERT = os.getenv('CLIENT_CERT',   '/certs/actuation-service.crt')
CLIENT_KEY = os.getenv('CLIENT_KEY',    '/certs/actuation-service.key')
RECONNECT_DELAY = 5

# Water threshold settings
WATER_ON_THRESHOLD = 1500
WATER_OFF_THRESHOLD = 1480
HIGH_PERSISTENCE_SECONDS = 3
WINDOW_SIZE = 5
RAINDROP_ANALOG_THRESHOLD = 1500

class ActuationService:

    def __init__(self, broker, port, topics):
        self.broker = broker
        self.port = port
        self.topics = topics
        self.client = mqtt.Client(client_id="actuation-service", clean_session=True)

        # mTLS — present our cert so the broker can verify our identity
        # CA cert verifies the broker, CLIENT_CERT + CLIENT_KEY is our identity
        self.client.tls_set(
            ca_certs=CA_CERT,
            certfile=CLIENT_CERT,
            keyfile=CLIENT_KEY,
            tls_version=ssl.PROTOCOL_TLS_CLIENT
        )
        logger.info(f"LOG - INFO: mTLS enabled (cert: {CLIENT_CERT})")

        # Callbacks
        self.client.on_connect = self.on_connect
        self.client.on_disconnect = self.on_disconnect
        self.client.on_message = self.on_message
        
        # Enable automatic reconnection
        self.client.reconnect_delay_set(min_delay=1, max_delay=120)

        # Per-device actuation state for hysteresis, persistence, and anomaly detection
        self.device_state = defaultdict(lambda: {
            'high_start_time': None,
            'recent_readings': deque(maxlen=WINDOW_SIZE),
            'last_command': 'deactivate'
        })


    def _get_device_state(self, device_id):
        return self.device_state[device_id]

    def is_raindrop_detected(self, analog, digital):
        if digital is not None:
            if str(digital) in ('1', 'true', 'True', 'yes', 'Yes'):
                return True

        try:
            return int(analog) >= RAINDROP_ANALOG_THRESHOLD
        except (TypeError, ValueError):
            return False

    def on_connect(self, client, userdata, flags, rc):
        """
        Callback when connected to broker
        
        """
        if rc == 0:
            logger.info(f"LOG - INFO: Connected to MQTT broker at {self.broker}:{self.port}")

            # TODO Subscribe to topics
            for topic in self.topics:
                client.subscribe(topic, qos=1)
                logger.info(f"LOG - INFO: Subscribed to topic: {topic}")

        elif rc == 5:
            logger.error("LOG - ERROR: Connection refused - Certificate rejected by broker")
            sys.exit(1)
        else:
            logger.error(f"LOG - ERROR: Connection failed with code {rc}")


    def on_disconnect(self, client, userdata, rc):
        """
        Callback when disconnected from broker
        
        """

        if rc != 0:
            logger.warning(f"LOG - WARNING: Unexpected disconnection (code {rc}). Reconnecting...")
        else:
            logger.info("LOG - INFO: Disconnected from broker")


    def on_message(self, client, userdata, msg):
        """
        Callback when message is received
        
        """
        timestamp = datetime.now(timezone.utc).isoformat() + 'Z'
        topic = msg.topic
        payload = msg.payload.decode('utf-8')

        # Extract device_id from topic path: devices/{device_id}/water_level
        topic_parts = topic.split('/')
        device_id = topic_parts[1] if len(topic_parts) >= 2 else 'unknown'

        logger.info(f"{'='*80}")
        logger.info(f"LOG - INFO: MESSAGE RECEIVED")
        logger.info(f"LOG - INFO: Timestamp: {timestamp}")
        logger.info(f"LOG - INFO: Topic:     {topic}")
        logger.info(f"LOG - INFO: Device:    {device_id}")
        logger.info(f"LOG - INFO: Payload:   {payload}")

        # Route message based on topic -> To be expanded based on data collected
        if 'water_level' in topic:
            self.handle_water_level(payload, timestamp, device_id)
        else:
            logger.warning(f"LOG - WARNING: Unknown topic: {topic}")

        logger.info(f"{'='*80}\n")


    def handle_water_level(self, payload, timestamp, device_id):
        """
        Analyse water level data and publish an actuation command.
        """
        try:
            data = json.loads(payload)

            if 'water_level' in data:
                water_sensor_value = data['water_level']
                sensor_label = 'water_level'
            elif 'water_sensor' in data:
                water_sensor_raw = data['water_sensor']
                # Handle nested dict format: {"raw": value}
                if isinstance(water_sensor_raw, dict) and 'raw' in water_sensor_raw:
                    water_sensor_value = water_sensor_raw['raw']
                else:
                    water_sensor_value = water_sensor_raw
                sensor_label = 'water_sensor'
            else:
                raise ValueError('Payload missing water_level or water_sensor field')

            raindrop_data = data.get('raindrop_sensor', {})
            raindrop_analog = raindrop_data.get('analog')
            raindrop_digital = raindrop_data.get('digital')

            logger.info(f"LOG - INFO: Analysing {sensor_label} for device {device_id}: {water_sensor_value}")
            logger.info(f"LOG - INFO: raindrop_sensor analog={raindrop_analog} digital={raindrop_digital}")

            water_level = float(water_sensor_value)
            raindrop_confirmed = self.is_raindrop_detected(raindrop_analog, raindrop_digital)
            now = datetime.now(timezone.utc)

            state = self._get_device_state(device_id)
            state['recent_readings'].append({
                'water_level': water_level,
                'raindrop': raindrop_confirmed,
                'timestamp': now
            })

            recent_high_count = sum(1 for r in state['recent_readings'] if r['water_level'] >= WATER_ON_THRESHOLD)
            recent_rain_count = sum(1 for r in state['recent_readings'] if r['raindrop'])
            logger.info(
                f"LOG - INFO: Window highs={recent_high_count}/{len(state['recent_readings'])} "
                f"rain confirms={recent_rain_count}/{len(state['recent_readings'])}"
            )

            if water_level < WATER_OFF_THRESHOLD:
                state['high_start_time'] = None
            elif state['high_start_time'] is None and water_level >= WATER_ON_THRESHOLD:
                state['high_start_time'] = now

            sustained_seconds = 0.0
            if state['high_start_time'] is not None:
                sustained_seconds = (now - state['high_start_time']).total_seconds()

            if water_level >= WATER_ON_THRESHOLD and not raindrop_confirmed and recent_high_count < 2:
                logger.warning("LOG - WARNING: Possible short spike detected")

            if raindrop_confirmed and water_level < WATER_OFF_THRESHOLD:
                logger.warning("LOG - WARNING: Rain detected without sufficient water rise yet")

            command = None
            reason = None
            activate = False

            if water_level >= WATER_ON_THRESHOLD:
                if raindrop_confirmed:
                    activate = True
                    reason = "high water confirmed by raindrop sensor"
                elif sustained_seconds >= HIGH_PERSISTENCE_SECONDS:
                    activate = True
                    reason = "sustained high water level"
                elif recent_high_count >= 3:
                    activate = True
                    reason = "persistently high water readings"
                else:
                    reason = "high water detected, waiting for confirmation"
            elif state['last_command'] == 'activate' and water_level > WATER_OFF_THRESHOLD:
                activate = True
                reason = "hysteresis hold"
            elif raindrop_confirmed and water_level >= WATER_OFF_THRESHOLD:
                activate = True
                reason = "raindrop confirmed with water near threshold"
            else:
                reason = "water normal"

            pump_state = 1 if activate else 0
            command = {
                "pump": pump_state,
                "servo": 90,
                "capture_image": False,
                "poll_interval": 500,
                "reason": reason
            }
            if activate:
                state['last_command'] = 'activate'
            else:
                state['last_command'] = 'deactivate'

            self.publish_command(device_id, command)

        except json.JSONDecodeError:
            logger.error("LOG - ERROR: Invalid JSON payload — cannot determine actuation")
        except ValueError as e:
            logger.error(f"LOG - ERROR: {e} — cannot determine actuation")


    def publish_command(self, device_id, command):
        """
        Publish an actuation command to the device's actuate topic.

        """

        topic = f"devices/{device_id}/actuate"
        payload = json.dumps(command)
        result = self.client.publish(topic, payload, qos=1)

        if result.rc == mqtt.MQTT_ERR_SUCCESS:
            logger.info(f"LOG - INFO: Command published → {topic}: {payload}")
        else:
            logger.error(f"LOG - ERROR: Failed to publish command to {topic} (rc={result.rc})")


    def connect_with_retry(self):
        """
        Retry Connection.
        Infinitely loops until connection is successful.
        
        """

        while True:
            try:
                logger.info(f"LOG - INFO: Attempting to connect to {self.broker}:{self.port}...")
                self.client.connect(self.broker, self.port, keepalive=60)
                break
            except Exception as e:
                logger.error(f"LOG - ERROR: Connection failed: {e}")
                logger.info(f"LOG - INFO: Retrying in {RECONNECT_DELAY} seconds...")
                time.sleep(RECONNECT_DELAY)


    def start(self):
        logger.info("=" * 80)
        logger.info("LOG - INFO: ACTUATION SERVICE STARTING")
        logger.info("=" * 80)
        logger.info(f"LOG - INFO: Broker: {self.broker}:{self.port}")
        logger.info(f"LOG - INFO: Topics: {', '.join(self.topics)}")
        logger.info(f"LOG - INFO: TLS:    mTLS (cert: {CLIENT_CERT})")
        logger.info("=" * 80)

        self.connect_with_retry()

        try:
            self.client.loop_forever()
        except KeyboardInterrupt:
            logger.info("LOG - INFO: Shutting down gracefully...")
            self.client.disconnect()
            self.client.loop_stop()


if __name__ == "__main__":
    service = ActuationService(MQTT_BROKER, MQTT_PORT, MQTT_TOPICS)
    service.start()
