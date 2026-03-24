#!/usr/bin/env python3
"""
Actuation Logic Service
Subscribes to devices/+/water_level, analyses the payload,
and publishes a command to devices/{device_id}/actuate.

TODO: Add decision logic in handle_water_level()
"""

import os
import sys
import ssl
import time
import json
import logging
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

        TODO: Replace the stub logic below with real decision logic.
              The command dict can be extended with whatever fields the device needs.
              We could have it upon reaching a set threshold, increase polling rate, call the Modelled actuation
        """
        try:
            data = json.loads(payload)
            water_level = data.get('water_level')

            logger.info(f"LOG - INFO: Analysing water level for device {device_id}: {water_level}")

            # --- TODO: decision logic goes here ---
            command = {
                "deviceId": device_id,
                "ts": timestamp,
                "action": "none",    # TODO: replace with derived action e.g. "pump_on"
                "reason": "stub"     # TODO: replace with derived reason
            }
            # --------------------------------------

            self.publish_command(device_id, command)

        except json.JSONDecodeError:
            logger.error("LOG - ERROR: Invalid JSON payload — cannot determine actuation")


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
