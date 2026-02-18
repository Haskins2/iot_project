#!/usr/bin/env python3
"""
MQTT Subscriber Service with Authentication
Subscribes to controller/telemetry Topic.
Auto reconnects.
"""

import os
import sys
import time
import json
import logging
from datetime import datetime
import paho.mqtt.client as mqtt

# Logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[logging.StreamHandler(sys.stdout)]
)
logger = logging.getLogger('mqtt-subscriber')

# ENV based config
MQTT_BROKER = os.getenv('MQTT_BROKER', 'mosquitto')
MQTT_PORT = int(os.getenv('MQTT_PORT', '1883'))
# TODO Add aditional topics 
MQTT_TOPICS = os.getenv('MQTT_TOPICS', 'controller/telemetry').split(',')
MQTT_USERNAME = os.getenv('MQTT_USERNAME')  
MQTT_PASSWORD = os.getenv('MQTT_PASSWORD')  
RECONNECT_DELAY = 5  # 5 seconds

class MQTTSubscriber:

    def __init__(self, broker, port, topics, username=None, password=None):
        self.broker = broker
        self.port = port
        self.topics = topics
        self.username = username
        self.password = password
        self.client = mqtt.Client(client_id="subscriber-service", clean_session=False)
        
        if self.username and self.password:
            self.client.username_pw_set(self.username, self.password)
            logger.info(f"LOG - INFO: Authentication enabled for user: {self.username}")
        else:
            logger.warning("LOG - WARNING: No authentication credentials provided")
        
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
            logger.error(f"LOG - ERROR: Connection refused - Authentication failed (invalid username/password)")
            sys.exit(1)
        else:
            logger.error(f"LOG - ERROR: Connection failed with code {rc}")
            

    def on_disconnect(self, client, userdata, rc):
        """
        Callback when disconnected from broker
        
        """

        if rc != 0:
            logger.warning(f"LOG - WARNING: Unexpected disconnection from broker (code {rc}). Reconnecting...")
        else:
            logger.info("LOG - INFO: Disconnected from broker")
            

    def on_message(self, client, userdata, msg):
        """
        Callback when message is received
        
        """

        timestamp = datetime.now(datetime.timezone.utc).isoformat() + 'Z'
        topic = msg.topic
        payload = msg.payload.decode('utf-8')
        
        # Log messages 
        logger.info(f"{'='*80}")
        logger.info(f"LOG - INFO: MESSAGE RECEIVED")
        logger.info(f"LOG - INFO: Timestamp: {timestamp}")
        logger.info(f"LOG - INFO: Topic: {topic}")
        logger.info(f"LOG - INFO: Payload: {payload}")
        
        # Route message based on topic -> To be exapanded based on data collected
        if topic == "controller/telemetry":
            self.handle_telemetry(payload, timestamp)
        else:
            logger.warning(f"LOG - WARNING: Unknown topic: {topic}")
            
        # '=====' break for easier log message view
        logger.info(f"{'='*80}\n")

    def handle_telemetry(self, payload, timestamp):
        """
        Handle telemetry data. TODO This could be changed to instead just forward this data to a microservice, for now its "acting"
        as if it is processing the telemetry data by printing to the logs. See comments on what it would be if it was a handling
        processing itself
        
        """

        logger.info(f"LOG - INFO: TELEMETRY RECEIVED")

        # TODO 
        try:
            data = json.loads(payload)
            logger.info(f"LOG - INFO: Device ID: {data.get('deviceId', 'N/A')}")
            logger.info(f"LOG - INFO: Temperature: {data.get('temperature', 'N/A')}°C")
            logger.info(f"LOG - INFO: Status: {data.get('status', 'N/A')}")
            logger.info(f"LOG - INFO: Telemetry TS: {data.get('ts', 'N/A')}")
            
            """
            1. Capture read data
            2. Transform into format accepted by actuation logic service
            3. Pass data to actuation logic service
            4. Store Data into DB
            """
            
        except json.JSONDecodeError:
            logger.error(f"LOG - ERROR: Invalid JSON payload")
            

    def connect_with_retry(self):
        """
        Retry Connetion.
        Infintely loops until connection is successful.
        
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
        """
        Start the subscriber service.
        
        """

        logger.info("=" * 80)
        logger.info("LOG - INFO: MQTT SUBSCRIBER SERVICE STARTING")
        logger.info("=" * 80)
        logger.info(f"LOG - INFO: Broker: {self.broker}:{self.port}")
        logger.info(f"LOG - INFO: Topics: {', '.join(self.topics)}")
        logger.info(f"LOG - INFO: Auth: {'Enabled' if self.username else 'Disabled'}")
        logger.info("=" * 80)
        
        self.connect_with_retry()
        
        # Start the loop (blocking, handles reconnection automatically)
        try:
            self.client.loop_forever()
        except KeyboardInterrupt:
            logger.info("LOG - INFO: Shutting down gracefully...")
            self.client.disconnect()
            self.client.loop_stop()

if __name__ == "__main__":
    subscriber = MQTTSubscriber(
        MQTT_BROKER, 
        MQTT_PORT, 
        MQTT_TOPICS,
        username=MQTT_USERNAME,
        password=MQTT_PASSWORD
    )
    subscriber.start()