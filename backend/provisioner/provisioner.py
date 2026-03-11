#!/usr/bin/env python3
"""
Provisioning Service
--------------------
mqtt client

On boot:
  - Connects to broker as provisioning-service (dynsec admin)
  - Subscribes to $provision/register
  - Subscribes to $CONTROL/dynamic-security/v1/response (dynsec replies)

When a device publishes to $provision/register:
  { "device_id": "esp-001", "hmac": "<sha256 hex>" }

  1. Validate HMAC-SHA256(BOOTSTRAP_SECRET, device_id)
  2. Send dynsec createClient (or modifyClient if device already exists)
  3. Assign device to device-role
  4. Reply to device on $provision/response/{device_id} with opaque token
  5. Schedule dynsec deleteClient after TOKEN_TTL_HOURS via threading.Timer

Token = secrets.token_hex(16)  — 32 hex chars, 128 bits
The token IS the dynsec password for that device client.
Mosquitto validates it on every CONNECT, no extra lookup needed.
"""