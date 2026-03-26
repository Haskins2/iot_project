#!/usr/bin/env python3
"""
Dummy Actuation Logic Service for Testing
Simulates receiving MQTT messages with fake payloads and shows responses.
No actual MQTT connection required.
"""

import json
import logging
import sys
from datetime import datetime, timezone

# Logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[logging.StreamHandler(sys.stdout)]
)
logger = logging.getLogger('dummy-actuation-service')

# Threshold for triggering an actuation command
ALERT_THRESHOLD = 19
certainty = 0
class FakeMessage:
    def __init__(self, topic, payload):
        self.topic = topic
        self.payload = payload.encode('utf-8')

class DummyActuationService:
    def __init__(self):
        self.client = None  # No real client

    def on_message(self, client, userdata, msg):
        """
        Callback when message is received (simulated)
        """
        timestamp = datetime.now(timezone.utc).isoformat() + 'Z'
        topic = msg.topic
        payload = msg.payload.decode('utf-8')

        # Extract device_id from topic path: devices/{device_id}/water_level
        topic_parts = topic.split('/')
        device_id = topic_parts[1] if len(topic_parts) >= 2 else 'unknown'

        logger.info(f"{'='*80}")
        logger.info(f"LOG - INFO: SIMULATED MESSAGE RECEIVED")
        logger.info(f"LOG - INFO: Timestamp: {timestamp}")
        logger.info(f"LOG - INFO: Topic:     {topic}")
        logger.info(f"LOG - INFO: Device:    {device_id}")
        logger.info(f"LOG - INFO: Payload:   {payload}")
        logger.info(f"LOG - INFO: certainty:   {certainty}")

        # Route message based on topic
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
            water_level = data.get('water_level')

            logger.info(f"LOG - INFO: Analysing water level for device {device_id}: {water_level}")

            # Decision logic
            water_level = float(data["water_level"])
            if water_level >= ALERT_THRESHOLD:
                global certainty
                certainty = certainty + 0.1
                command = {"deviceId": device_id, "ts": timestamp, "action": "pump_on", "reason": "water above threshold"}
            else:
                command = {"deviceId": device_id, "ts": timestamp, "action": "pump_off", "reason": "water normal"}

            self.publish_command(device_id, command)

        except json.JSONDecodeError:
            logger.error("LOG - ERROR: Invalid JSON payload — cannot determine actuation")

    def publish_command(self, device_id, command):
        """
        Simulate publishing an actuation command (just print it).
        """
        topic = f"devices/{device_id}/actuate"
        payload = json.dumps(command)

        logger.info(f"LOG - INFO: SIMULATED COMMAND PUBLISHED → {topic}: {payload}")

    def simulate_message(self, topic, payload):
        """
        Simulate receiving a message.
        """
        msg = FakeMessage(topic, payload)
        self.on_message(None, None, msg)

def main():
    service = DummyActuationService()

    print("Dummy Actuation Service Tester")
    print("Enter water level values to test (or 'quit' to exit):")
    print(f"Threshold: {ALERT_THRESHOLD}")
    print()

    while True:
        try:
            user_input = input("Water level: ").strip()
            if user_input.lower() == 'quit':
                break

            # Parse input as float
            water_level = float(user_input)

            # Create fake payload
            payload = json.dumps({"water_level": water_level})

            # Simulate message for device "test_device"
            service.simulate_message("devices/test_device/water_level", payload)

        except ValueError:
            print("Invalid input. Please enter a number or 'quit'.")
        except KeyboardInterrupt:
            print("\nExiting...")
            break

if __name__ == "__main__":
    main()